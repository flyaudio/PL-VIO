#include <stdio.h>
#include <queue>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <ros/ros.h>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/opencv.hpp>

#include "estimator.h"
#include "log.hpp"
#include "parameters.h"
#include "utility/visualization.h"
#include "loop-closure/loop_closure.h"
#include "loop-closure/keyframe.h"
#include "loop-closure/keyframe_database.h"
#include "camodocal/camera_models/CameraFactory.h"
#include "camodocal/camera_models/CataCamera.h"
#include "camodocal/camera_models/PinholeCamera.h"

Estimator g_estimator;

std::condition_variable con;
double current_time = -1;
queue<sensor_msgs::ImuConstPtr> g_imu_buf;
queue<sensor_msgs::PointCloudConstPtr> g_feature_buf;
queue<sensor_msgs::PointCloudConstPtr> g_linefeature_buf;
std::mutex m_posegraph_buf;
queue<int> optimize_posegraph_buf;
queue<KeyFrame*> keyframe_buf;
queue<RetriveData> retrive_data_buf;

int sum_of_wait = 0;

std::mutex m_buf;
std::mutex m_state;
std::mutex i_buf;
std::mutex m_loop_drift;
std::mutex m_keyframedatabase_resample;
std::mutex m_update_visualization;
std::mutex m_keyframe_buf;
std::mutex m_retrive_data_buf;

double latest_time;
Eigen::Vector3d tmp_P;
Eigen::Quaterniond tmp_Q;
Eigen::Vector3d tmp_V;
Eigen::Vector3d tmp_Ba;
Eigen::Vector3d tmp_Bg;
Eigen::Vector3d acc_0;
Eigen::Vector3d gyr_0;

queue<pair<cv::Mat, double>> image_buf;
LoopClosure *loop_closure;
KeyFrameDatabase keyframe_database;

int global_frame_cnt = 0;
//camera param
camodocal::CameraPtr m_camera;
vector<int> erase_index;
std_msgs::Header cur_header;
Eigen::Vector3d relocalize_t{Eigen::Vector3d(0, 0, 0)};
Eigen::Matrix3d relocalize_r{Eigen::Matrix3d::Identity()};

class Measurement {
public:
    std::vector<sensor_msgs::ImuConstPtr> imu_datas;
    sensor_msgs::PointCloudConstPtr pt_features;
    sensor_msgs::PointCloudConstPtr line_features;
};


/*
  使用mid-point方法对imu状态量进行预测
*/
void predict(const sensor_msgs::ImuConstPtr &imu_msg)
{
    double t = imu_msg->header.stamp.toSec();
    double dt = t - latest_time;
    latest_time = t;

    double dx = imu_msg->linear_acceleration.x;
    double dy = imu_msg->linear_acceleration.y;
    double dz = imu_msg->linear_acceleration.z;
    Eigen::Vector3d linear_acceleration{dx, dy, dz};

    double rx = imu_msg->angular_velocity.x;
    double ry = imu_msg->angular_velocity.y;
    double rz = imu_msg->angular_velocity.z;
    Eigen::Vector3d angular_velocity{rx, ry, rz};

    Eigen::Vector3d un_acc_0 = tmp_Q * (acc_0 - tmp_Ba - tmp_Q.inverse() * g_estimator.g);  // Qwi * (ai - ba - Qiw * g)

    Eigen::Vector3d un_gyr = 0.5 * (gyr_0 + angular_velocity) - tmp_Bg;                   // (gyro0 + gyro1)/2 - bg
    tmp_Q = tmp_Q * Utility::deltaQ(un_gyr * dt);                                         // Qwj = Qwi * [1, 1/2 w dt]

    Eigen::Vector3d un_acc_1 = tmp_Q * (linear_acceleration - tmp_Ba - tmp_Q.inverse() * g_estimator.g);// Qwj * (aj - ba - Qjw * g);其中j=i+1

    Eigen::Vector3d un_acc = 0.5 * (un_acc_0 + un_acc_1);//转到word坐标,就可以直接相加.前后两次数据取平均值

    tmp_P = tmp_P + dt * tmp_V + 0.5 * dt * dt * un_acc;
    tmp_V = tmp_V + dt * un_acc;

    acc_0 = linear_acceleration;
    gyr_0 = angular_velocity;
}

void update()
{
    TicToc t_predict;
    latest_time = current_time;
    tmp_P = relocalize_r * g_estimator.Ps[WINDOW_SIZE] + relocalize_t;
    tmp_Q = relocalize_r * g_estimator.Rs[WINDOW_SIZE];
    tmp_V = g_estimator.Vs[WINDOW_SIZE];
    tmp_Ba = g_estimator.Bas[WINDOW_SIZE];
    tmp_Bg = g_estimator.Bgs[WINDOW_SIZE];
    acc_0 = g_estimator.acc_0;
    gyr_0 = g_estimator.gyr_0;

    queue<sensor_msgs::ImuConstPtr> tmp_imu_buf = g_imu_buf;
    for (sensor_msgs::ImuConstPtr tmp_imu_msg; !tmp_imu_buf.empty(); tmp_imu_buf.pop())
        predict(tmp_imu_buf.front());

}

//<imu, <feature, linefeature>>
std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>,
        std::pair<sensor_msgs::PointCloudConstPtr,sensor_msgs::PointCloudConstPtr> >>
getMeasurements()
{
    std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>,
            std::pair<sensor_msgs::PointCloudConstPtr,sensor_msgs::PointCloudConstPtr> >> measurements;

    while (true)
    {
        if (g_imu_buf.empty() || g_feature_buf.empty() || g_linefeature_buf.empty())
            return measurements;//return empty

        std::cout<<"-------------------------------------\n";
        if (!(g_imu_buf.back()->header.stamp > g_feature_buf.front()->header.stamp)) //如果imu最新数据的时间戳不大于最旧图像的时间戳，那得等imu数据
        {
            ROS_WARN("wait for imu, only should happen at the beginning");
            sum_of_wait++;
            return measurements;//return empty
        }

        if (!(g_imu_buf.front()->header.stamp < g_feature_buf.front()->header.stamp)) // 如果imu最老的数据时间戳不小于最旧图像的时间，那得把最老的图像丢弃
        {
            ROS_WARN("throw img, only should happen at the beginning");
            g_feature_buf.pop();
            g_linefeature_buf.pop();
            continue;
        }
        sensor_msgs::PointCloudConstPtr img_msg = g_feature_buf.front();
        g_feature_buf.pop();
        sensor_msgs::PointCloudConstPtr linefeature_msg = g_linefeature_buf.front();
        g_linefeature_buf.pop();

        // 遍历两个图像之间所有的imu数据
        std::vector<sensor_msgs::ImuConstPtr> IMUs;
        while (g_imu_buf.front()->header.stamp <= img_msg->header.stamp)
        {
            IMUs.emplace_back(g_imu_buf.front());
            g_imu_buf.pop();
        }
        measurements.emplace_back(IMUs, std::make_pair(img_msg,linefeature_msg) );
    }
    return measurements;
}

std::vector<Measurement> getMeasurements_matt() {
    std::vector<Measurement> measurements;

    while (true) {
        if (g_imu_buf.empty() || g_feature_buf.empty() || g_linefeature_buf.empty()) {
            return measurements;//untill empty
        }

        if (!(g_imu_buf.back()->header.stamp > g_feature_buf.front()->header.stamp)) //如果imu最新数据的时间戳不大于最旧图像的时间戳，那得等imu数据
        {
            LOGW("wait for imu, only should happen at the beginning");
            sum_of_wait++;
            return measurements;//untill empty
        }

        if (!(g_imu_buf.front()->header.stamp < g_feature_buf.front()->header.stamp)) // 如果imu最老的数据时间戳不小于最旧图像的时间，那得把最老的图像丢弃
        {
            LOGW("throw img, only should happen at the beginning");
            g_feature_buf.pop();
            g_linefeature_buf.pop();
            continue;
        }
        Measurement m;
        m.pt_features = g_feature_buf.front();
        g_feature_buf.pop();
        m.line_features = g_linefeature_buf.front();
        g_linefeature_buf.pop();

        // 遍历两个图像之间所有的imu数据
        while (g_imu_buf.front()->header.stamp <= m.pt_features->header.stamp)
        {
            m.imu_datas.emplace_back(g_imu_buf.front());
            g_imu_buf.pop();
        }
        measurements.emplace_back(m);
    }
    LOGI("measurements size:{}", measurements.size());
    return measurements;
}


void imu_callback(const sensor_msgs::ImuConstPtr &imu_msg)
{
    m_buf.lock();
    g_imu_buf.push(imu_msg);
    m_buf.unlock();
    con.notify_one();

    {
        std::lock_guard<std::mutex> lg(m_state);
        predict(imu_msg);
        if (g_estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR) {
            std_msgs::Header header = imu_msg->header;
            header.frame_id = "world";
           pubLatestOdometry(tmp_P, tmp_Q, tmp_V, header);
        }
    }
}

void raw_image_callback(const sensor_msgs::ImageConstPtr &img_msg)
{
    cv_bridge::CvImagePtr img_ptr = cv_bridge::toCvCopy(img_msg, sensor_msgs::image_encodings::MONO8);
    //image_pool[img_msg->header.stamp.toNSec()] = img_ptr->image;
    if(LOOP_CLOSURE)
    {
        i_buf.lock();
        image_buf.push(make_pair(img_ptr->image, img_msg->header.stamp.toSec()));
        i_buf.unlock();
    }
}

void feature_callback(const sensor_msgs::PointCloudConstPtr &feature_msg)
{
    m_buf.lock();
    g_feature_buf.push(feature_msg);
    m_buf.unlock();
    con.notify_one();
}

void linefeature_callback(const sensor_msgs::PointCloudConstPtr &feature_msg)
{
    m_buf.lock();
    g_linefeature_buf.push(feature_msg);
    m_buf.unlock();
    con.notify_one();
}

void send_imu(const sensor_msgs::ImuConstPtr &imu_msg)
{
    double t = imu_msg->header.stamp.toSec();
    if (current_time < 0)
        current_time = t;
    double dt = t - current_time;
    current_time = t;

    double ba[]{0.0, 0.0, 0.0};
    double bg[]{0.0, 0.0, 0.0};

    double dx = imu_msg->linear_acceleration.x - ba[0];
    double dy = imu_msg->linear_acceleration.y - ba[1];
    double dz = imu_msg->linear_acceleration.z - ba[2];

    double rx = imu_msg->angular_velocity.x - bg[0];
    double ry = imu_msg->angular_velocity.y - bg[1];
    double rz = imu_msg->angular_velocity.z - bg[2];

    g_estimator.processIMU(dt, Vector3d(dx, dy, dz), Vector3d(rx, ry, rz));
}

// thread: visual-inertial odometry
void process()
{
    while (true)
    {
        //<imu, [point, line]>
        std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>,
                std::pair<sensor_msgs::PointCloudConstPtr,sensor_msgs::PointCloudConstPtr> >> measurements;
        std::unique_lock<std::mutex> lk(m_buf);
        con.wait(lk, [&]
                 {
            // return (measurements = getMeasurements()).size() != 0;
            measurements = getMeasurements();
            return !measurements.empty();
                 });
        lk.unlock();

        for (auto &measurement : measurements)
        {
            // 处理imu数据, 预测 pose
            for (auto &imu_msg : measurement.first) {
                send_imu(imu_msg);                     
            }
            auto point_and_line_msg = measurement.second;
            auto img_msg = point_and_line_msg.first;
            auto line_msg = point_and_line_msg.second;
            LOGI("processing vision data with stamp {}", img_msg->header.stamp.toSec());

            //handle point feature
            TicToc t_s;
            map<int, vector<pair<int, Vector3d>>> image;//<featureID, vector<camID, 归一化坐标>>
            for (unsigned int i = 0; i < img_msg->points.size(); i++)
            {
                int v = img_msg->channels[0].values[i] + 0.5;
                int feature_id = v / NUM_OF_CAM;
                int camera_id = v % NUM_OF_CAM;        // 被几号相机观测到的，如果是单目，camera_id = 0
                double x = img_msg->points[i].x;
                double y = img_msg->points[i].y;
                double z = img_msg->points[i].z;
                ROS_ASSERT(z == 1);
                image[feature_id].emplace_back(camera_id, Vector3d(x, y, z));
            }
            
            //handle line feature
            map<int, vector<pair<int, Vector4d>>> lines;//<featureID, vector<camID, 线段两端点>
            for (unsigned int i = 0; i < line_msg->points.size(); i++)
            {
                int v = line_msg->channels[0].values[i] + 0.5;
                int feature_id = v / NUM_OF_CAM;
                int camera_id = v % NUM_OF_CAM;        // 被几号相机观测到的，如果是单目，camera_id = 0
                double x_startpoint = line_msg->points[i].x;
                double y_startpoint = line_msg->points[i].y;
                double x_endpoint = line_msg->channels[1].values[i];
                double y_endpoint = line_msg->channels[2].values[i];
                // ROS_ASSERT(z == 1);
                lines[feature_id].emplace_back(camera_id, Vector4d(x_startpoint, y_startpoint, x_endpoint, y_endpoint));
            }

            // 处理image数据，这时候的image已经是特征点数据，不是原始图像了。
            g_estimator.processImage(image, lines, img_msg->header);   
  
            double whole_t = t_s.toc();
            printStatistics(g_estimator, whole_t);
            std_msgs::Header header = img_msg->header;
            header.frame_id = "world";
            cur_header = header;
            m_loop_drift.lock();

            pubOdometry(g_estimator, header, relocalize_t, relocalize_r);
            pubKeyPoses(g_estimator, header, relocalize_t, relocalize_r);
            pubCameraPose(g_estimator, header, relocalize_t, relocalize_r);
            pubLinesCloud(g_estimator, header, relocalize_t, relocalize_r);
            pubPointCloud(g_estimator, header, relocalize_t, relocalize_r);
            pubTF(g_estimator, header, relocalize_t, relocalize_r);
            m_loop_drift.unlock();
            //ROS_ERROR("end: %f, at %f", img_msg->header.stamp.toSec(), ros::Time::now().toSec());
        }
        m_buf.lock();
        m_state.lock();
        if (g_estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR)
            update();
        m_state.unlock();
        m_buf.unlock();
    }
}

void process_matt() {
    while (true) {
        std::vector<Measurement> measurements;
        std::unique_lock<std::mutex> lk(m_buf);
        con.wait(lk, [&] {
                            return (measurements = getMeasurements_matt()).size() != 0;
                        });
        lk.unlock();

        for (auto &measurement : measurements)
        {
            // 处理imu数据, 预测 pose
            for (const auto& imu_msg : measurement.imu_datas) {
                send_imu(imu_msg);                     
            }
            auto img_msg = measurement.pt_features;
            auto line_msg = measurement.line_features;
            LOGI("processing vision data with stamp {}", img_msg->header.stamp.toSec());

            //handle point feature
            TicToc t_s;
            map<int, vector<pair<int, Vector3d>>> image;//<featureID, vector<camID, 归一化坐标>>
            for (unsigned int i = 0; i < img_msg->points.size(); i++)
            {
                int v = img_msg->channels[0].values[i] + 0.5;
                int feature_id = v / NUM_OF_CAM;
                int camera_id = v % NUM_OF_CAM;        // 被几号相机观测到的，如果是单目，camera_id = 0
                double x = img_msg->points[i].x;
                double y = img_msg->points[i].y;
                double z = img_msg->points[i].z;
                ROS_ASSERT(z == 1);
                image[feature_id].emplace_back(camera_id, Vector3d(x, y, z));
            }
            
            //handle line feature
            map<int, vector<pair<int, Vector4d>>> lines;//<featureID, vector<camID, 线段两端点>
            for (unsigned int i = 0; i < line_msg->points.size(); i++)
            {
                int v = line_msg->channels[0].values[i] + 0.5;
                int feature_id = v / NUM_OF_CAM;
                int camera_id = v % NUM_OF_CAM;        // 被几号相机观测到的，如果是单目，camera_id = 0
                double x_startpoint = line_msg->points[i].x;
                double y_startpoint = line_msg->points[i].y;
                double x_endpoint = line_msg->channels[1].values[i];
                double y_endpoint = line_msg->channels[2].values[i];
                // ROS_ASSERT(z == 1);
                lines[feature_id].emplace_back(camera_id, Vector4d(x_startpoint, y_startpoint, x_endpoint, y_endpoint));
            }

            // 处理image数据，这时候的image已经是特征点数据，不是原始图像了。
            g_estimator.processImage(image, lines, img_msg->header);   
  
            double whole_t = t_s.toc();
            printStatistics(g_estimator, whole_t);
            std_msgs::Header header = img_msg->header;
            header.frame_id = "world";
            cur_header = header;
            m_loop_drift.lock();

            pubOdometry(g_estimator, header, relocalize_t, relocalize_r);
            pubKeyPoses(g_estimator, header, relocalize_t, relocalize_r);
            pubCameraPose(g_estimator, header, relocalize_t, relocalize_r);
            pubLinesCloud(g_estimator, header, relocalize_t, relocalize_r);
            pubPointCloud(g_estimator, header, relocalize_t, relocalize_r);
            pubTF(g_estimator, header, relocalize_t, relocalize_r);
            m_loop_drift.unlock();
            //ROS_ERROR("end: %f, at %f", img_msg->header.stamp.toSec(), ros::Time::now().toSec());
        }
        m_buf.lock();
        m_state.lock();
        if (g_estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR) {
            update();
        }
        m_state.unlock();
        m_buf.unlock();
    }
}


int main(int argc, char **argv)
{
    ros::init(argc, argv, "vins_estimator");
    // ros::console::shutdown();
    ros::NodeHandle n("~");
    if(ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Debug)) {
        ros::console::notifyLoggerLevelsChanged();
    }
    readParameters(n);
    g_estimator.setParameter();
#ifdef EIGEN_DONT_PARALLELIZE
    LOGI("EIGEN_DONT_PARALLELIZE");
#endif
    LOGI("waiting for image and imu...");

    registerPub(n);
    ros::Subscriber sub_imu = n.subscribe(IMU_TOPIC, 2000, imu_callback, ros::TransportHints().tcpNoDelay());
    ros::Subscriber sub_image = n.subscribe("/feature_tracker/feature", 2000, feature_callback);
    ros::Subscriber sub_linefeature = n.subscribe("/linefeature_tracker/linefeature", 2000, linefeature_callback);
    ros::Subscriber sub_raw_image = n.subscribe(IMAGE_TOPIC, 2000, raw_image_callback);

    // thread: visual-inertial odometry
    std::thread measurement_process{process_matt};
    ros::spin();

    return 0;
}
