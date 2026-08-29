/**
 * @file    heartbeat.cpp
 * @brief   心跳机制实现。
 */

#include "heartbeat.h"
#include "common/logger.h"

#include <chrono>
#include <algorithm>

namespace ara {
namespace com {
namespace safety {

static std::uint64_t now_ms() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count());
}

// =========================================================================
//  心跳发送器
// =========================================================================

HeartbeatProvider::~HeartbeatProvider() {
    stop();
}

void HeartbeatProvider::start(std::uint32_t interval_ms, SendCallback send_cb) {
    if (m_running.load()) {
        ARA_LOG_WARN("HB", "HeartbeatProvider already running");
        return;
    }

    m_interval_ms = interval_ms;
    m_send_cb = std::move(send_cb);
    m_running.store(true);
    m_thread = std::thread(&HeartbeatProvider::loop, this);

    ARA_LOG_INFO("HB", "HeartbeatProvider started (interval=" +
                 std::to_string(interval_ms) + "ms)");
}

void HeartbeatProvider::stop() {
    if (!m_running.load()) return;

    m_running.store(false);
    m_wait_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    ARA_LOG_INFO("HB", "HeartbeatProvider stopped");
}

void HeartbeatProvider::loop() {
    while (m_running.load()) {
        std::unique_lock<std::mutex> lock(m_wait_mutex);
        if (m_wait_cv.wait_for(lock, std::chrono::milliseconds(m_interval_ms),
                               [this] { return !m_running.load(); })) break;
        lock.unlock();

        if (m_send_cb) {
            m_send_cb();
        }
    }
}

// =========================================================================
//  心跳监控器
// =========================================================================

HeartbeatMonitor::~HeartbeatMonitor() {
    stop();
}

void HeartbeatMonitor::start(std::uint32_t timeout_ms, TimeoutCallback on_timeout) {
    if (m_running.load()) {
        ARA_LOG_WARN("HB", "HeartbeatMonitor already running");
        return;
    }

    m_timeout_ms = timeout_ms;
    m_timeout_cb = std::move(on_timeout);
    m_last_heartbeat_ms.store(now_ms());
    m_running.store(true);
    m_thread = std::thread(&HeartbeatMonitor::loop, this);

    ARA_LOG_INFO("HB", "HeartbeatMonitor started (timeout=" +
                 std::to_string(timeout_ms) + "ms)");
}

void HeartbeatMonitor::stop() {
    if (!m_running.load()) return;

    m_running.store(false);
    m_wait_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    ARA_LOG_INFO("HB", "HeartbeatMonitor stopped");
}

void HeartbeatMonitor::on_heartbeat_received() {
    m_last_heartbeat_ms.store(now_ms());
}

void HeartbeatMonitor::loop() {
    while (m_running.load()) {
        std::unique_lock<std::mutex> lock(m_mutex);
        const auto check_interval = std::max<std::uint32_t>(1U, m_timeout_ms / 2U);
        if (m_wait_cv.wait_for(lock, std::chrono::milliseconds(check_interval),
                               [this] { return !m_running.load(); })) break;
        lock.unlock();

        std::uint64_t elapsed = now_ms() - m_last_heartbeat_ms.load();
        if (elapsed > m_timeout_ms) {
            ARA_LOG_WARN("HB", "Heartbeat timeout! Last HB " +
                         std::to_string(elapsed) + "ms ago (limit=" +
                         std::to_string(m_timeout_ms) + "ms)");
            if (m_timeout_cb) {
                m_timeout_cb();
            }
            // 重置计时，避免重连前重复触发
            m_last_heartbeat_ms.store(now_ms());
        }
    }
}

} // namespace safety
} // namespace com
} // namespace ara
