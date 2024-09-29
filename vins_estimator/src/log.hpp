#pragma once
#include <iostream>
#include <sstream>
#include <iomanip>
#include <string>
#include <memory>
#include <time.h>
#include <chrono>
#include <stdlib.h> /* abort, NULL*/
#include <boost/filesystem.hpp>
#include "spdlog/spdlog.h"
#include "spdlog/fmt/ostr.h"
#include "spdlog/async.h"
#include "spdlog/sinks/ansicolor_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/rotating_file_sink.h"
//https://github.com/gabime/spdlog/wiki/3.-Custom-formatting#pattern-flags

class XLogger {
public:
    static XLogger* getInst() {
        static XLogger xlogger;
        return &xlogger;
    }

    spdlog::logger* getLoggerPtr() {
        return m_logger.get();
    }

    std::shared_ptr<spdlog::logger> getLoggerShrdPtr() const {
        return m_logger;
    }

private:
    // make ctor private to avoid outside instance
    XLogger()
    {
        auto getTimeStr = []() {
            auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::stringstream ss;
            // ss << std::put_time(std::localtime(&t), "%Y%m%d_%H");//"%Y%m%d_%H%M%S"
            ss << std::put_time(std::localtime(&t), "%Y%m%d_%H%M%S");
            return ss.str();
        };
        // hardcode log path
         boost::filesystem::path curPath = boost::filesystem::current_path();
        //  boost::filesystem::path logPath = curPath / "../../user_data/other/shelf_logs";
         boost::filesystem::path logPath = curPath;
        if( !boost::filesystem::exists(logPath) ){
            boost::filesystem::create_directories(logPath);
        }

        const std::string log_dir = logPath.string(); // should create the folder if not exist
        const std::string logger_name_prefix = "shelf_";

        std::string level = "debug";

        try {
            // logger name with timestamp
            std::string logger_name = logger_name_prefix + std::string(getTimeStr());

            spdlog::enable_backtrace(32); // Store the latest 32 messages in a buffer
            spdlog::set_level(spdlog::level::debug);
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            console_sink->set_level(spdlog::level::info);
            console_sink->set_pattern("%^%m-%d %H:%M:%S.%e [%s:%#] %v%$");

            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(log_dir + "/" + logger_name + ".log", 50 * 1024 * 1024, 1000);
            file_sink->set_level(spdlog::level::debug);
//            file_sink->set_pattern("%Y-%m-%d %H:%M:%S.%e <%t> [%L] [%@] %v");
            file_sink->set_pattern("%Y-%m-%d %H:%M:%S.%e <%t> [%L] [%s:%#] %v");

            std::vector<spdlog::sink_ptr> sinks;
            sinks.push_back(console_sink);
            sinks.push_back(file_sink);
            m_logger = std::make_shared<spdlog::logger>("console_and_file_logger", begin(sinks), end(sinks));
            m_logger->set_level(spdlog::level::debug);
            setErrorHandle();
            spdlog::register_logger(m_logger); //register it if you need to access it globally

            spdlog::flush_every(std::chrono::seconds(3));
            if (level == "trace") {
                m_logger->flush_on(spdlog::level::trace);
            }
            else if (level == "debug") {
                m_logger->flush_on(spdlog::level::debug);
            }
            else if (level == "info") {
                m_logger->flush_on(spdlog::level::info);
            }
            else if (level == "warn") {
                m_logger->flush_on(spdlog::level::warn);
            }
            else if (level == "error") {
                m_logger->flush_on(spdlog::level::err);
            }
        }
        catch (const spdlog::spdlog_ex& ex) {
            std::cout << "log init failed: " << ex.what() << std::endl;
        }
        std::cout << "log ctor" << std::endl;
    }

    ~XLogger() {
        spdlog::drop_all(); // close all logger
        std::cout << "log dtor " << std::endl;
    }

    XLogger(const XLogger&) = delete;
    XLogger& operator=(const XLogger&) = delete;

    void setErrorHandle() {
        m_logger->set_error_handler([](const std::string &msg) {
            spdlog::get("console_and_file_logger")->error("*** LOGGER ERR ***: {}", msg);
        });

    }

private:
    std::shared_ptr<spdlog::logger> m_logger;
};

#define SPDLOGT(...) SPDLOG_LOGGER_CALL(XLogger::getInst()->getLoggerPtr(), spdlog::level::trace, __VA_ARGS__);
#define SPDLOGD(...) SPDLOG_LOGGER_CALL(XLogger::getInst()->getLoggerPtr(), spdlog::level::debug, __VA_ARGS__);
#define SPDLOGI(...) SPDLOG_LOGGER_CALL(XLogger::getInst()->getLoggerPtr(), spdlog::level::info, __VA_ARGS__);
#define SPDLOGW(...) SPDLOG_LOGGER_CALL(XLogger::getInst()->getLoggerPtr(), spdlog::level::warn, __VA_ARGS__);
#define SPDLOGE(...) SPDLOG_LOGGER_CALL(XLogger::getInst()->getLoggerPtr(), spdlog::level::err, __VA_ARGS__);

#define LOGT(...) SPDLOG_LOGGER_CALL(XLogger::getInst()->getLoggerPtr(), spdlog::level::trace, __VA_ARGS__);
#define LOGD(...) SPDLOG_LOGGER_CALL(XLogger::getInst()->getLoggerPtr(), spdlog::level::debug, __VA_ARGS__);
#define LOGI(...) SPDLOG_LOGGER_CALL(XLogger::getInst()->getLoggerPtr(), spdlog::level::info, __VA_ARGS__);
#define LOGW(...) SPDLOG_LOGGER_CALL(XLogger::getInst()->getLoggerPtr(), spdlog::level::warn, __VA_ARGS__);
#define LOGE(...) SPDLOG_LOGGER_CALL(XLogger::getInst()->getLoggerPtr(), spdlog::level::err, __VA_ARGS__);
/**/

/* disable log
#define LOGT(...)
#define LOGD(...)
#define LOGI(...)
#define LOGW(...)
#define LOGE(...)
*/

#ifndef ASST
#define ASST(x)   do{ \
                if(!(x)) { \
                    SPDLOGE("Assert failed at %s:%d", __FILE__, __LINE__); \
                    SPDLOGE("    : " #x ); \
                    abort(); \
                } \
            }while(0)
#endif

//#define DEPRECATED