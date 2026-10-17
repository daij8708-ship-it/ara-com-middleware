/**
 * @file    monitor.cpp
 * @brief   运行监测指标与 JSON 快照实现。
 */

#include "monitor.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace ara {
namespace com {
namespace monitor {

namespace {

constexpr const char* kCounterNames[] = {
    "rpc_submitted",
    "rpc_ok",
    "rpc_timeout",
    "rpc_unavailable",
    "rpc_failed",
    "rpc_request_bytes",
    "rpc_response_bytes",
    "rpc_latency_us_total",
    "rpc_latency_samples",
    "rpc_latency_max_us",
    "pending_calls",
    "events_sent",
    "events_send_failed",
    "events_sent_bytes",
    "events_received",
    "events_received_bytes",
    "events_delivered",
    "events_duplicate",
    "events_corrupt",
    "event_seq_gaps",
    "event_seq_missing",
    "resume_requests",
    "resume_replayed",
    "resume_gaps",
    "resume_failures",
    "reconnects",
    "disconnects",
    "heartbeat_timeouts",
    "methods_handled",
    "method_failures",
    "idempotency_hits",
    "service_available",
};

static_assert(sizeof(kCounterNames) / sizeof(kCounterNames[0]) ==
                  static_cast<std::size_t>(Counter::Count),
              "指标名数量必须与 Counter::Count 一致");

std::uint64_t process_id() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

std::string iso8601_utc(std::chrono::system_clock::time_point point) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(point);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &seconds);
#else
    gmtime_r(&seconds, &tm_buf);
#endif
    std::ostringstream out;
    out << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string json_escape(const std::string& text) {
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

std::string safe_component(const std::string& text) {
    std::string out;
    for (const char ch : text) {
        const bool accepted = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        out.push_back(accepted ? ch : '_');
    }
    return out.empty() ? "process" : out;
}

} // namespace

const char* counter_name(Counter counter) {
    const auto idx = static_cast<std::size_t>(counter);
    return idx < static_cast<std::size_t>(Counter::Count) ? kCounterNames[idx] : "unknown";
}

const char* com_ret_name(ComRet code) {
    switch (code) {
        case ComRet::OK:            return "OK";
        case ComRet::TIMEOUT:       return "TIMEOUT";
        case ComRet::NOT_AVAILABLE: return "NOT_AVAILABLE";
        case ComRet::E2E_ERROR:     return "E2E_ERROR";
        case ComRet::FRAGMENT_ERR:  return "FRAGMENT_ERR";
        case ComRet::INTERNAL_ERR:  return "INTERNAL_ERR";
        case ComRet::NOT_CONNECTED: return "NOT_CONNECTED";
        default:                    return "UNKNOWN";
    }
}

const char* com_ret_description(ComRet code) {
    switch (code) {
        case ComRet::OK:            return "调用成功";
        case ComRet::TIMEOUT:       return "在超时时间内未收到响应";
        case ComRet::NOT_AVAILABLE: return "服务未发布或当前不可用";
        case ComRet::E2E_ERROR:     return "端到端校验失败";
        case ComRet::FRAGMENT_ERR:  return "分片或重组失败";
        case ComRet::INTERNAL_ERR:  return "框架内部错误";
        case ComRet::NOT_CONNECTED: return "连接尚未建立";
        default:                    return "未知错误码";
    }
}

Monitor::Monitor() {
    for (auto& counter : m_counters) counter.store(0, std::memory_order_relaxed);
}

Monitor& Monitor::instance() {
    static Monitor monitor;
    return monitor;
}

void Monitor::configure(ProcessInfo info) {
    std::lock_guard<std::mutex> lock(m_info_mutex);
    m_info = std::move(info);
}

void Monitor::inc(Counter counter, std::uint64_t delta) {
    m_counters[index(counter)].fetch_add(delta, std::memory_order_relaxed);
}

void Monitor::set(Counter counter, std::uint64_t value) {
    m_counters[index(counter)].store(value, std::memory_order_relaxed);
}

std::uint64_t Monitor::get(Counter counter) const {
    return m_counters[index(counter)].load(std::memory_order_relaxed);
}

void Monitor::complete_rpc(ComRet status, std::uint64_t latency_us,
                           std::size_t response_bytes) {
    inc(Counter::RpcResponseBytes, response_bytes);
    inc(Counter::RpcLatencyUsTotal, latency_us);
    inc(Counter::RpcLatencySamples);

    std::uint64_t current_max = m_counters[index(Counter::RpcLatencyMaxUs)].load(
        std::memory_order_relaxed);
    while (latency_us > current_max &&
           !m_counters[index(Counter::RpcLatencyMaxUs)].compare_exchange_weak(
               current_max, latency_us, std::memory_order_relaxed)) {
    }

    // 在途数只减不增；异常路径可能重复完成，因此避免下溢。
    std::uint64_t pending = m_counters[index(Counter::PendingCalls)].load(
        std::memory_order_relaxed);
    while (pending > 0 &&
           !m_counters[index(Counter::PendingCalls)].compare_exchange_weak(
               pending, pending - 1, std::memory_order_relaxed)) {
    }

    switch (status) {
        case ComRet::OK:
            inc(Counter::RpcOk);
            break;
        case ComRet::TIMEOUT:
            inc(Counter::RpcTimeout);
            break;
        case ComRet::NOT_AVAILABLE:
        case ComRet::NOT_CONNECTED:
            inc(Counter::RpcUnavailable);
            break;
        default:
            inc(Counter::RpcFailed);
            break;
    }
}

std::string Monitor::snapshot_json() const {
    ProcessInfo info;
    {
        std::lock_guard<std::mutex> lock(m_info_mutex);
        info = m_info;
    }

    const auto now = std::chrono::system_clock::now();
    const auto uptime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               now - m_started_at).count();

    std::ostringstream out;
    out << "{\"schema\":\"ara-com-monitor-v1\""
        << ",\"role\":\"" << json_escape(info.role) << "\""
        << ",\"backend\":\"" << json_escape(info.backend) << "\""
        << ",\"service_id\":" << info.service_id
        << ",\"instance_id\":" << info.instance_id
        << ",\"pid\":" << process_id()
        << ",\"started_at\":\"" << iso8601_utc(m_started_at) << "\""
        << ",\"updated_at\":\"" << iso8601_utc(now) << "\""
        << ",\"uptime_ms\":" << uptime_ms
        << ",\"stopped\":" << (m_stopped.load() ? "true" : "false")
        << ",\"counters\":{";
    for (std::size_t i = 0; i < static_cast<std::size_t>(Counter::Count); ++i) {
        if (i != 0) out << ',';
        out << '"' << kCounterNames[i] << "\":"
            << m_counters[i].load(std::memory_order_relaxed);
    }
    out << "},\"derived\":{";

    const auto submitted = get(Counter::RpcSubmitted);
    const auto ok = get(Counter::RpcOk);
    const auto samples = get(Counter::RpcLatencySamples);
    const auto latency_total = get(Counter::RpcLatencyUsTotal);
    const double ok_rate = submitted == 0
                               ? 0.0
                               : static_cast<double>(ok) / static_cast<double>(submitted);
    const auto avg_latency = samples == 0 ? 0 : latency_total / samples;
    out << "\"rpc_ok_rate\":" << std::fixed << std::setprecision(4) << ok_rate
        << ",\"rpc_avg_latency_us\":" << avg_latency
        << '}';

    out << '}';
    return out.str();
}

bool Monitor::start_snapshot_writer(const std::string& directory,
                                    std::uint32_t interval_ms) {
    {
        std::lock_guard<std::mutex> lock(m_writer_mutex);
        if (m_writer_thread.joinable() || m_writer_starting) return false;

        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) return false;

        std::string role;
        {
            std::lock_guard<std::mutex> info_lock(m_info_mutex);
            role = safe_component(m_info.role);
        }

        m_snapshot_directory = directory;
        m_snapshot_path = (std::filesystem::path(directory) /
                           ("metrics-" + role + "-" + std::to_string(process_id()) + ".json"))
                              .string();
        m_snapshot_interval_ms = std::max<std::uint32_t>(100, interval_ms);
        m_writer_stop.store(false);
        m_writer_starting = true;
    }

    // 线程在锁外创建：新线程第一件事就是获取同一把锁，避免启动阶段互相等待。
    std::thread worker([this]() {
        {
            std::unique_lock<std::mutex> wait_lock(m_writer_mutex);
            while (!m_writer_stop.load()) {
                m_writer_cv.wait_for(wait_lock,
                                     std::chrono::milliseconds(m_snapshot_interval_ms));
                if (m_writer_stop.load()) break;
                wait_lock.unlock();
                write_snapshot_once();
                wait_lock.lock();
            }
        }
        // 退出循环时 wait_lock 已析构，最后一份快照才能重新获取写锁。
        write_snapshot_once();
    });

    {
        std::lock_guard<std::mutex> lock(m_writer_mutex);
        m_writer_thread = std::move(worker);
        m_writer_starting = false;
    }
    return true;
}

void Monitor::stop_snapshot_writer() {
    {
        std::lock_guard<std::mutex> lock(m_writer_mutex);
        if (!m_writer_thread.joinable()) return;
        m_writer_stop.store(true);
    }
    m_writer_cv.notify_all();
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(m_writer_mutex);
        worker = std::move(m_writer_thread);
    }
    if (worker.joinable()) worker.join();
}

std::string Monitor::snapshot_path() const {
    std::lock_guard<std::mutex> lock(m_writer_mutex);
    return m_snapshot_path;
}

void Monitor::write_snapshot_once() const {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(m_writer_mutex);
        path = m_snapshot_path;
    }
    if (path.empty()) return;

    const std::string temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::out | std::ios::trunc);
        if (!out.is_open()) return;
        out << snapshot_json() << '\n';
        out.flush();
        if (!out.good()) return;
    }

    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        // Windows 上目标文件存在时 rename 可能失败，退化为删除后重试。
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
    }
}

Monitor::~Monitor() {
    stop_snapshot_writer();
}

} // namespace monitor
} // namespace com
} // namespace ara
