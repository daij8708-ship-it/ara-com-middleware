/**
 * @file    demo_observability.h
 * @brief   演示工具的可观测性接入：按环境变量写 JSONL 日志与指标快照。
 *
 * 环境变量全部显式开启，未设置时保持原有行为：
 *   ARA_COM_LOG_DIR              JSONL 日志目录
 *   ARA_COM_LOG_ROTATE_BYTES     单文件上限，默认 2 MiB
 *   ARA_COM_LOG_MAX_FILES        保留的历史文件数，默认 5
 *   ARA_COM_METRICS_DIR          指标快照目录
 *   ARA_COM_METRICS_INTERVAL_MS  快照周期，默认 500 ms
 */
#pragma once

#include "common/logger.h"
#include "common/monitor.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace demo {

inline std::uint32_t env_u32(const char* name, std::uint32_t fallback) {
    const char* text = std::getenv(name);
    if (!text || *text == '\0') return fallback;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0') return fallback;
    return static_cast<std::uint32_t>(value);
}

class Observability {
public:
    Observability(const std::string& role, const std::string& backend,
                  ara::com::ServiceId service_id,
                  ara::com::InstanceId instance_id) {
        using namespace ara::com;
        monitor::Monitor::instance().configure({role, backend, service_id, instance_id});

        const char* log_dir = std::getenv("ARA_COM_LOG_DIR");
        if (log_dir && *log_dir) {
            std::error_code ec;
            std::filesystem::create_directories(log_dir, ec);
            logger::LogOptions options;
            options.json_lines = true;
            options.rotate_bytes = env_u32("ARA_COM_LOG_ROTATE_BYTES", 2U * 1024U * 1024U);
            options.max_files = env_u32("ARA_COM_LOG_MAX_FILES", 5);
            options.component = role;
            options.backend = backend;
            const auto path = std::filesystem::path(log_dir) / (role + ".jsonl");
            logger::AsyncLogger::instance().init(
                LogLevel::INFO, path.string(), 8192, options);
        }

        const char* metrics_dir = std::getenv("ARA_COM_METRICS_DIR");
        if (metrics_dir && *metrics_dir) {
            monitor::Monitor::instance().start_snapshot_writer(
                metrics_dir, env_u32("ARA_COM_METRICS_INTERVAL_MS", 500));
        }
    }

    ~Observability() {
        auto& monitor = ara::com::monitor::Monitor::instance();
        monitor.mark_stopped();
        monitor.stop_snapshot_writer();
        ara::com::logger::AsyncLogger::instance().shutdown();
    }

    Observability(const Observability&) = delete;
    Observability& operator=(const Observability&) = delete;
};

} // namespace demo
