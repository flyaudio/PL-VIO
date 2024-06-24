//
// Created by hyj on 17-12-8.
//

#ifndef VINS_ESTIMATOR_LINE_GEOMETRY_H
#define VINS_ESTIMATOR_LINE_GEOMETRY_H
#include <eigen3/Eigen/Dense>
using namespace Eigen;
typedef Matrix<double,6,1> Vector6d;
typedef Matrix<double,8,1> Vector8d;
typedef Matrix<double,6,6> Matrix6d;

Vector4d line_to_orth(Vector6d line);
Vector6d orth_to_line(Vector4d orth);
Vector4d plk_to_orth(Vector6d plk);
Vector6d orth_to_plk(Vector4d orth);

/**
 * get plane from 3 points
*/
Vector4d pi_from_ppp(Vector3d x1, Vector3d x2, Vector3d x3);
Vector6d pipi_plk( Vector4d pi1, Vector4d pi2);
// 获取光心到直线的垂直点
Vector3d plucker_origin(Vector3d n, Vector3d v);
Matrix3d skew_symmetric( Vector3d v );

/**
 * @return Pworld = camTworld.inverse() * Pcam
*/
Vector3d poit_from_pose( Eigen::Matrix3d Rcw, Eigen::Vector3d tcw, Vector3d pt_c );
/**
 * @return Pcam = camTworld * Pworld
*/
Vector3d point_to_pose( Eigen::Matrix3d Rcw, Eigen::Vector3d tcw , Vector3d pt_w );
/**
 * 将空间直线的参数进行两个坐标系下的转换，其中线参数前3维是光心到直线的垂直点
*/
Vector6d line_to_pose(Vector6d line_w, Eigen::Matrix3d Rcw, Eigen::Vector3d tcw);
Vector6d line_from_pose(Vector6d line_c, Eigen::Matrix3d Rcw, Eigen::Vector3d tcw);

/**
 * @return 相机坐标系下的plk坐标
 */ 
Vector6d plk_to_pose( Vector6d plk_w, Eigen::Matrix3d Rcw, Eigen::Vector3d tcw );
/**
 * @return 世界坐标系下的plk坐标
*/
Vector6d plk_from_pose( Vector6d plk_c, Eigen::Matrix3d Rcw, Eigen::Vector3d tcw );

#endif //VINS_ESTIMATOR_LINE_GEOMETRY_H
