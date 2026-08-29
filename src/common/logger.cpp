/**
 * @file    logger.cpp
 * @brief   异步分级日志实现。
 */

#include "logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>

namespace ara {
namespace com {
namespace logger {

// =========================================================================
//  格式化时间戳
// =========================================================================

static std::string format_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_buf);
#endif

    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
       << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

// =========================================================================
//  将日志级别转换为字符串
// =========================================================================

static const char* level_to_string(LogLevel lv) {
    switch (lv) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
        default:              return "????";
    }
}

// =========================================================================
//  异步日志器
// =========================================================================

AsyncLogger& AsyncLogger::instance() {
    static AsyncLogger logger;
    return logger;
}

bool AsyncLogger::init(LogLevel min_level, const std::string& log_file_path, std::size_t max_queue) {
    if (m_initialized.load()) {
        return false;  // Already initialized
    }

    m_min_level.store(min_level);
    m_max_queue = max_queue;

    // 以追加模式打开日志文件
    m_log_file.open(log_file_path, std::ios::out | std::ios::app);
    if (!m_log_file.is_open()) {
        std::cerr << "[LOGGER] Failed to open log file: " << log_file_path << std::endl;
        return false;
    }

    m_running.store(true);
    m_initialized.store(true);

    // 启动后台写线程
    m_writer_thread = std::thread(&AsyncLogger::writer_thread, this);

    // 写入启动标记
    log(LogLevel::INFO, "LOGGER", "Logger initialized. Level=" +
        std::string(level_to_string(min_level)) + " File=" + log_file_path);

    return true;
}

void AsyncLogger::shutdown() {
    if (!m_initialized.load()) return;

    log(LogLevel::INFO, "LOGGER", "Logger shutting down.");
    m_running.store(false);
    m_queue_cv.notify_all();

    if (m_writer_thread.joinable()) {
        m_writer_thread.join();
    }

    m_initialized.store(false);
}

void AsyncLogger::set_level(LogLevel level) {
    m_min_level.store(level);
}

void AsyncLogger::log(LogLevel level, const std::string& tag, const std::string& message) {
    if (!m_initialized.load()) {
        // 日志器初始化前回退到标准错误输出
        std::cerr << "[" << level_to_string(level) << "][" << tag << "] " << message << std::endl;
        return;
    }

    if (level < m_min_level.load()) {
        return;  // Filtered out
    }

    LogEntry entry;
    entry.level     = level;
    entry.tag       = tag;
    entry.message   = message;
    entry.timestamp = format_timestamp();

    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        if (m_queue.size() < m_max_queue) {
            m_queue.push(std::move(entry));
        }
        // 队列已满时丢弃记录，保证业务线程不阻塞
    }
    m_queue_cv.notify_one();
}

void AsyncLogger::writer_thread() {
    while (m_running.load()) {
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        m_queue_cv.wait(lock, [this] {
            return !m_queue.empty() || !m_running.load();
        });

        // 清空待写队列
        while (!m_queue.empty()) {
            const LogEntry& entry = m_queue.front();

            // 格式：[时间] [级别] [标签] 消息
            std::ostringstream line;
            line << "[" << entry.timestamp << "] "
                 << "[" << level_to_string(entry.level) << "] "
                 << "[" << entry.tag << "] "
                 << entry.message;

            // 写入控制台，警告及以上级别使用标准错误输出
            if (entry.level >= LogLevel::WARN) {
                std::cerr << line.str() << std::endl;
            } else {
                std::cout << line.str() << std::endl;
            }

            // 写入文件
            {
                std::lock_guard<std::mutex> file_lock(m_file_mutex);
                if (m_log_file.is_open()) {
                    m_log_file << line.str() << std::endl;
                    m_log_file.flush();  // Flush immediately for crash resilience
                }
            }

            m_queue.pop();
        }
        lock.unlock();
    }

    // 停止前写完剩余记录
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        while (!m_queue.empty()) {
            const LogEntry& entry = m_queue.front();
            std::ostringstream line;
            line << "[" << entry.timestamp << "] "
                 << "[" << level_to_string(entry.level) << "] "
                 << "[" << entry.tag << "] "
                 << entry.message;
            std::cout << line.str() << std::endl;
            if (m_log_file.is_open()) {
                m_log_file << line.str() << std::endl;
            }
            m_queue.pop();
        }
    }
}

AsyncLogger::~AsyncLogger() {
    shutdown();
}

} // namespace logger
} // namespace com
} // namespace ara
