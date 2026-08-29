/**
 * @file    heartbeat.h
 * @brief   连接心跳保活机制。
 *
 * 服务端周期发送心跳，客户端监控超时；超时后触发连接丢失回调。
 *
 */
#pragma once

#include "common/types.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace ara {
namespace com {
namespace safety {

// =========================================================================
//  心跳发送器（服务端）
// =========================================================================

/**
 * @class HeartbeatProvider
 * @brief 周期发送心跳信号。
 *
 * 后台线程按配置周期调用发送回调。
 */
class HeartbeatProvider {
public:
    /// 心跳发送回调
    using SendCallback = std::function<void()>;

    HeartbeatProvider() = default;
    ~HeartbeatProvider();

    HeartbeatProvider(const HeartbeatProvider&) = delete;
    HeartbeatProvider& operator=(const HeartbeatProvider&) = delete;

    /**
     * @brief 启动周期心跳发送。
     * @param interval_ms 心跳间隔，单位毫秒。
     * @param send_cb 周期调用的心跳发送回调。
     */
    void start(std::uint32_t interval_ms, SendCallback send_cb);

    /// 停止心跳发送
    void stop();

private:
    void loop();

    std::atomic<bool> m_running{false};
    std::uint32_t m_interval_ms{1000};
    SendCallback  m_send_cb;
    std::thread   m_thread;
    std::mutex m_wait_mutex;
    std::condition_variable m_wait_cv;
};

// =========================================================================
//  心跳监控器（客户端）
// =========================================================================

/**
 * @class HeartbeatMonitor
 * @brief 监控心跳并检测连接丢失。
 *
 * 客户端每次收到心跳都应调用 `on_heartbeat_received()` 更新状态；超时后调用连接丢失回调。
 */
class HeartbeatMonitor {
public:
    /// 心跳超时回调
    using TimeoutCallback = std::function<void()>;

    HeartbeatMonitor() = default;
    ~HeartbeatMonitor();

    HeartbeatMonitor(const HeartbeatMonitor&) = delete;
    HeartbeatMonitor& operator=(const HeartbeatMonitor&) = delete;

    /**
     * @brief 启动心跳监控。
     * @param timeout_ms 判定丢失前允许的最大间隔。
     * @param on_timeout 超时回调。
     */
    void start(std::uint32_t timeout_ms, TimeoutCallback on_timeout);

    /// 停止监控
    void stop();

    /// 每次收到心跳时由客户端调用
    void on_heartbeat_received();

private:
    void loop();

    std::atomic<bool> m_running{false};
    std::atomic<std::uint64_t> m_last_heartbeat_ms{0};
    std::uint32_t m_timeout_ms{3000};
    TimeoutCallback m_timeout_cb;
    std::thread m_thread;
    std::mutex  m_mutex;
    std::condition_variable m_wait_cv;
};

} // namespace safety
} // namespace com
} // namespace ara
