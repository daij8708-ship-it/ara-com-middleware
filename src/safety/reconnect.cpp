/**
 * @file    reconnect.cpp
 * @brief   指数退避自动重连实现。
 */

#include "reconnect.h"
#include "common/logger.h"

#include <chrono>
#include <algorithm>

namespace ara {
namespace com {
namespace safety {

ReconnectManager::~ReconnectManager() {
    stop();
}

void ReconnectManager::configure(std::uint32_t initial_delay_ms,
                                  std::uint32_t max_delay_ms,
                                  std::uint32_t multiplier) {
    m_initial_delay_ms = std::max<std::uint32_t>(1U, initial_delay_ms);
    m_max_delay_ms = std::max(m_initial_delay_ms, max_delay_ms);
    m_multiplier = std::max<std::uint32_t>(1U, multiplier);
}

void ReconnectManager::set_callbacks(ConnectCallback try_connect,
                                      ReconnectedCallback on_reconnected) {
    m_connect_cb = std::move(try_connect);
    m_reconnected_cb = std::move(on_reconnected);
}

void ReconnectManager::on_connection_lost() {
    if (!m_connected.load()) {
        return;  // 已处于重连状态
    }

    m_connected.store(false);
    ARA_LOG_WARN("RECON", "Connection lost, starting reconnection...");

    // 停止已有重连线程
    if (m_thread.joinable()) {
        m_running.store(false);
        m_wait_cv.notify_all();
        m_thread.join();
    }

    m_running.store(true);
    m_thread = std::thread(&ReconnectManager::reconnect_loop, this);
}

void ReconnectManager::on_connection_ok() {
    const bool was_connected = m_connected.exchange(true);
    m_wait_cv.notify_all();
    if (!was_connected && m_reconnected_cb) m_reconnected_cb();
}

void ReconnectManager::stop() {
    m_running.store(false);
    m_wait_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void ReconnectManager::reconnect_loop() {
    std::uint32_t attempt = 0;

    while (m_running.load() && !m_connected.load()) {
        std::uint32_t delay = m_initial_delay_ms;
        for (std::uint32_t i = 0; i < attempt; ++i) {
            delay *= m_multiplier;
            if (delay > m_max_delay_ms) {
                delay = m_max_delay_ms;
                break;
            }
        }

        ARA_LOG_INFO("RECON", "Reconnect attempt " + std::to_string(attempt + 1) +
                     " (delay=" + std::to_string(delay) + "ms)");

        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_wait_cv.wait_for(lock, std::chrono::milliseconds(delay), [this] {
                return !m_running.load() || m_connected.load();
            })) break;
        lock.unlock();

        // 尝试重连
        if (m_connect_cb && m_connect_cb()) {
            ARA_LOG_INFO("RECON", "Reconnection succeeded after " +
                         std::to_string(attempt + 1) + " attempt(s)");
            m_connected.store(true);
            if (m_reconnected_cb) {
                m_reconnected_cb();
            }
            return;
        }

        ++attempt;
    }
}

} // namespace safety
} // namespace com
} // namespace ara
