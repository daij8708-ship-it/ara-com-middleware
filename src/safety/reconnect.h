/**
 * @file    reconnect.h
 * @brief   使用指数退避的自动重连机制。
 *
 * 检测到心跳超时或 SOME/IP-SD 服务消失后，使用指数退避重新建立连接。
 *
 * 退避公式：delay = min(initial_delay * multiplier^attempt, max_delay)
 *
 */
#pragma once

#include "common/types.h"

#include <atomic>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace ara {
namespace com {
namespace safety {

/**
 * @class ReconnectManager
 * @brief 管理指数退避自动重连。
 *
 * 连接丢失后进入重连循环；每次尝试调用连接回调，恢复成功后返回空闲状态。
 */
class ReconnectManager {
public:
    /// 尝试重连的回调
    using ConnectCallback = std::function<bool()>;

    /// 连接恢复回调
    using ReconnectedCallback = std::function<void()>;

    ReconnectManager() = default;
    ~ReconnectManager();

    ReconnectManager(const ReconnectManager&) = delete;
    ReconnectManager& operator=(const ReconnectManager&) = delete;

    /**
     * @brief 配置重连参数。
     * @param initial_delay_ms 初始重试间隔。
     * @param max_delay_ms 最大重试间隔。
     * @param multiplier 退避倍数，通常为2。
     */
    void configure(std::uint32_t initial_delay_ms,
                   std::uint32_t max_delay_ms,
                   std::uint32_t multiplier);

    /**
     * @brief 注册回调。
     * @param try_connect 每次尝试时调用的函数。
     * @param on_reconnected 重连成功回调。
     */
    void set_callbacks(ConnectCallback try_connect, ReconnectedCallback on_reconnected);

    /// 检测到连接丢失时启动重连
    void on_connection_lost();

    /// 通知连接已恢复
    void on_connection_ok();

    /// 停止重连尝试
    void stop();

private:
    void reconnect_loop();

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{true};

    std::uint32_t m_initial_delay_ms{100};
    std::uint32_t m_max_delay_ms{5000};
    std::uint32_t m_multiplier{2};

    ConnectCallback     m_connect_cb;
    ReconnectedCallback m_reconnected_cb;

    std::thread m_thread;
    std::mutex  m_mutex;
    std::condition_variable m_wait_cv;
};

} // namespace safety
} // namespace com
} // namespace ara
