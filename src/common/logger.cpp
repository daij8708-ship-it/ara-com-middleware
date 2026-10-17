/**
 * @file    logger.cpp
 * @brief   异步分级日志实现。
 */

#include "logger.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace ara {
namespace com {
namespace logger {

// =========================================================================
//  格式化辅助
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

static const char* level_name(LogLevel lv) {
    switch (lv) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
        default:              return "UNKNOWN";
    }
}

static std::uint64_t process_id() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

static std::string json_escape(const std::string& text) {
    std::ostringstream out;
    for (const unsigned char ch : text) {
        switch (ch) {
            case '"':  out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(ch) << std::dec;
                } else {
                    out << static_cast<char>(ch);
                }
        }
    }
    return out.str();
}

// =========================================================================
//  异步日志器
// =========================================================================

AsyncLogger& AsyncLogger::instance() {
    static AsyncLogger logger;
    return logger;
}

bool AsyncLogger::init(LogLevel min_level, const std::string& log_file_path,
                       std::size_t max_queue, const LogOptions& options) {
    if (m_initialized.load()) {
        return false;  // 已经初始化
    }

    m_min_level.store(min_level);
    m_max_queue = max_queue;
    m_options = options;
    m_log_file_path = log_file_path;
    m_file_bytes = 0;
    m_dropped.store(0);

    // 以追加模式打开日志文件
    m_log_file.open(log_file_path, std::ios::out | std::ios::app);
    if (!m_log_file.is_open()) {
        std::cerr << "[LOGGER] Failed to open log file: " << log_file_path << std::endl;
        return false;
    }
    std::error_code size_error;
    m_file_bytes = static_cast<std::size_t>(
        std::filesystem::file_size(log_file_path, size_error));

    m_running.store(true);
    m_initialized.store(true);

    // 启动后台写线程
    m_writer_thread = std::thread(&AsyncLogger::writer_thread, this);

    // 写入启动标记
    log(LogLevel::INFO, "LOGGER", "Logger initialized. Level=" +
        std::string(level_name(min_level)) + " File=" + log_file_path +
        (options.json_lines ? " Format=jsonl" : " Format=text"));

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

    {
        std::lock_guard<std::mutex> file_lock(m_file_mutex);
        if (m_log_file.is_open()) m_log_file.close();
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
        return;  // 低于当前日志级别，跳过
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
        } else {
            // 队列已满时丢弃记录，保证业务线程不阻塞。
            m_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    m_queue_cv.notify_one();
}

std::string AsyncLogger::format_text(const LogEntry& entry) const {
    std::ostringstream line;
    line << "[" << entry.timestamp << "] "
         << "[" << level_to_string(entry.level) << "] "
         << "[" << entry.tag << "] "
         << entry.message;
    return line.str();
}

std::string AsyncLogger::format_file(const LogEntry& entry) const {
    if (!m_options.json_lines) return format_text(entry);

    std::ostringstream line;
    line << "{\"ts\":\"" << json_escape(entry.timestamp) << "\""
         << ",\"level\":\"" << level_name(entry.level) << "\""
         << ",\"component\":\"" << json_escape(m_options.component) << "\"";
    if (!m_options.backend.empty()) {
        line << ",\"backend\":\"" << json_escape(m_options.backend) << "\"";
    }
    line << ",\"tag\":\"" << json_escape(entry.tag) << "\""
         << ",\"pid\":" << process_id()
         << ",\"msg\":\"" << json_escape(entry.message) << "\"}";
    return line.str();
}

void AsyncLogger::rotate_if_needed(std::size_t incoming_bytes) {
    if (m_options.rotate_bytes == 0) return;
    if (m_file_bytes + incoming_bytes <= m_options.rotate_bytes) return;

    m_log_file.close();

    const std::filesystem::path base(m_log_file_path);
    std::error_code ec;
    if (m_options.max_files == 0) {
        std::filesystem::remove(base, ec);
    } else {
        std::filesystem::remove(base.string() + "." +
                                std::to_string(m_options.max_files), ec);
        for (std::size_t i = m_options.max_files - 1; i >= 1; --i) {
            const auto source = base.string() + "." + std::to_string(i);
            const auto target = base.string() + "." + std::to_string(i + 1);
            if (std::filesystem::exists(source, ec)) {
                std::filesystem::rename(source, target, ec);
            }
        }
        if (std::filesystem::exists(base, ec)) {
            std::filesystem::rename(base, base.string() + ".1", ec);
        }
    }

    m_log_file.open(m_log_file_path, std::ios::out | std::ios::app);
    m_file_bytes = 0;
}

void AsyncLogger::writer_thread() {
    std::unique_lock<std::mutex> lock(m_queue_mutex);
    while (true) {
        m_queue_cv.wait(lock, [this] {
            return !m_queue.empty() || !m_running.load();
        });

        while (!m_queue.empty()) {
            LogEntry entry = std::move(m_queue.front());
            m_queue.pop();
            lock.unlock();

            // 控制台保持人类可读的纯文本，文件可按配置写成 JSONL。
            const std::string console_line = format_text(entry);
            if (entry.level >= LogLevel::WARN) {
                std::cerr << console_line << std::endl;
            } else {
                std::cout << console_line << std::endl;
            }

            {
                std::lock_guard<std::mutex> file_lock(m_file_mutex);
                const std::string file_line = format_file(entry);
                rotate_if_needed(file_line.size() + 1);
                if (m_log_file.is_open()) {
                    m_log_file << file_line << '\n';
                    m_log_file.flush();  // 及时刷出，尽量减少异常退出时的丢失
                    m_file_bytes += file_line.size() + 1;
                }
            }

            lock.lock();
        }

        if (!m_running.load()) break;
    }
}

AsyncLogger::~AsyncLogger() {
    shutdown();
}

} // namespace logger
} // namespace com
} // namespace ara
