/**
 * @file    logger.h
 * @brief   异步分级日志组件。
 *
 * 线程安全的异步日志器，支持队列缓冲、控制台与文件输出以及分级过滤。
 *
 */
#pragma once

#include "types.h"

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>

namespace ara {
namespace com {
namespace logger {

// =========================================================================
//  日志记录结构
// =========================================================================

/// 包含时间、级别、标签和消息的单条日志
struct LogEntry {
    LogLevel     level;
    std::string  tag;      ///< Module/component tag (e.g. "SKELETON", "PROXY")
    std::string  message;
    std::string  timestamp;
};

// =========================================================================
//  异步日志器
// =========================================================================

/**
 * @class AsyncLogger
 * @brief 使用独立写线程的单例异步日志器。
 *
 * 调用方只负责日志入队，后台线程异步写入输出端，降低关键路径日志延迟。
 */
class AsyncLogger {
public:
    /// 获取单例
    static AsyncLogger& instance();

    /// 初始化日志器
    bool init(LogLevel min_level, const std::string& log_file_path, std::size_t max_queue = 4096);

    /// 平稳停止日志器
    void shutdown();

    /// 按指定级别记录消息
    void log(LogLevel level, const std::string& tag, const std::string& message);

    /// 设置最低日志级别
    void set_level(LogLevel level);

    /// 获取当前日志级别
    LogLevel level() const { return m_min_level.load(); }

    // ---- 便捷宏定义在类声明之后 ----

    ~AsyncLogger();

private:
    AsyncLogger() = default;
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    void writer_thread();  ///< 后台写入循环

    std::atomic<LogLevel>    m_min_level{LogLevel::INFO};
    std::atomic<bool>        m_running{false};
    std::atomic<bool>        m_initialized{false};

    std::queue<LogEntry>     m_queue;
    std::mutex               m_queue_mutex;
    std::condition_variable  m_queue_cv;
    std::size_t              m_max_queue{4096};

    std::thread              m_writer_thread;
    std::ofstream            m_log_file;
    std::mutex               m_file_mutex;
};

// =========================================================================
//  便捷日志宏
// =========================================================================

#define ARA_LOG_TRACE(tag, msg) \
    do { if (::ara::com::logger::AsyncLogger::instance().level() <= ::ara::com::LogLevel::TRACE) \
         ::ara::com::logger::AsyncLogger::instance().log(::ara::com::LogLevel::TRACE, tag, msg); } while(0)

#define ARA_LOG_DEBUG(tag, msg) \
    do { if (::ara::com::logger::AsyncLogger::instance().level() <= ::ara::com::LogLevel::DEBUG) \
         ::ara::com::logger::AsyncLogger::instance().log(::ara::com::LogLevel::DEBUG, tag, msg); } while(0)

#define ARA_LOG_INFO(tag, msg) \
    do { if (::ara::com::logger::AsyncLogger::instance().level() <= ::ara::com::LogLevel::INFO) \
         ::ara::com::logger::AsyncLogger::instance().log(::ara::com::LogLevel::INFO, tag, msg); } while(0)

#define ARA_LOG_WARN(tag, msg) \
    do { if (::ara::com::logger::AsyncLogger::instance().level() <= ::ara::com::LogLevel::WARN) \
         ::ara::com::logger::AsyncLogger::instance().log(::ara::com::LogLevel::WARN, tag, msg); } while(0)

#define ARA_LOG_ERROR(tag, msg) \
    ::ara::com::logger::AsyncLogger::instance().log(::ara::com::LogLevel::ERROR, tag, msg)

#define ARA_LOG_FATAL(tag, msg) \
    ::ara::com::logger::AsyncLogger::instance().log(::ara::com::LogLevel::FATAL, tag, msg)

// =========================================================================
//  基于流的格式化日志构造器
// =========================================================================

/// 使用流操作符构造日志消息
class LogStream {
public:
    LogStream(LogLevel level, std::string tag)
        : m_level(level), m_tag(std::move(tag)) {}

    ~LogStream() {
        AsyncLogger::instance().log(m_level, m_tag, m_ss.str());
    }

    template<typename T>
    LogStream& operator<<(const T& val) {
        m_ss << val;
        return *this;
    }

    LogStream(const LogStream&) = delete;
    LogStream& operator=(const LogStream&) = delete;

private:
    LogLevel     m_level;
    std::string  m_tag;
    std::ostringstream m_ss;
};

#define ARA_LOG_S(level, tag) ::ara::com::logger::LogStream(level, tag)

} // namespace logger
} // namespace com
} // namespace ara
