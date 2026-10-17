/**
 * @file    service_proxy.h
 * @brief   ServiceProxy：AP AUTOSAR ara::com 风格的客户端抽象。
 *
 * ServiceProxy 表示远端服务的客户端端点，封装服务发现、Event 订阅、
 * 同步/异步 Method 调用、E2E 校验、分片重组、心跳监控和自动重连。
 *
 * Usage:
 * @code
 *   ara::com::ServiceProxy proxy(backend, service_config);
 *   proxy.init();
 *   proxy.subscribe_event<MyData>(EVENT_ID, [](const MyData& data) { ... });
 *   proxy.start();
 *
 *   // 同步调用
 *   auto result = proxy.call_method_sync<ReqT, RspT>(METHOD_ID, request);
 *
 *   // 异步调用
 *   proxy.call_method_async<ReqT, RspT>(METHOD_ID, request,
 *       [](ComRet status, const RspT& rsp) { ... });
 * @endcode
 *
 */
#pragma once

#include "common/types.h"
#include "common/exception.h"
#include "common/logger.h"
#include "runtime.h"
#include "safety/e2e.h"
#include "safety/fragmentation.h"
#include "safety/heartbeat.h"
#include "safety/idempotency.h"
#include "safety/reconnect.h"
#include "safety/resume.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace ara {
namespace com {

/**
 * @class ServiceProxy
 * @brief 远端服务的客户端代理。
 *
 * 负责发现远端服务、订阅 Event，并提供同步及异步 Method 调用接口。
 */
class ServiceProxy {
public:
    /**
     * @brief 构造 ServiceProxy。
     * @param backend 通信后端。
     * @param config 服务配置，role 应为 client。
     * @param e2e_config E2E 保护配置。
     */
    ServiceProxy(std::shared_ptr<runtime::ICommunicationBackend> backend,
                 ServiceConfig config,
                 E2EConfig e2e_config = {});

    ~ServiceProxy();

    ServiceProxy(const ServiceProxy&) = delete;
    ServiceProxy& operator=(const ServiceProxy&) = delete;

    // ---- 生命周期 ----

    /// 初始化 Proxy
    bool init();

    /// 启动服务发现、Event 订阅和心跳监控
    bool start();

    /// 停止 Proxy
    void stop();

    // ---- Event 订阅 ----

    /**
     * @brief 订阅远端 Event。
     *
     * @tparam T 数据类型，必须提供静态 deserialize(bytes) 方法。
     * @param event_id Event 标识。
     * @param callback 收到 Event 时调用的回调。
     * @param group_id SOME/IP Event Group，默认值为1。
     */
    template<typename T>
    void subscribe_event(EventId event_id,
                         std::function<void(const T&)> callback,
                         EventGroupId group_id = 1);

    /// 使用原始字节回调订阅 Event
    void subscribe_event_raw(EventId event_id,
                             EventCallback callback,
                             EventGroupId group_id = 1);

    // ---- Method 调用 ----

    /**
     * @brief 同步调用远端 Method（阻塞）。
     *
     * @tparam ReqT 请求类型，必须提供 serialize()。
     * @tparam RspT 响应类型，必须提供静态 deserialize()。
     * @param method_id Method 标识。
     * @param request 请求数据。
     * @param timeout_ms 超时时间，单位毫秒。
     * @return 成功时返回响应对象。
     * @throws TimeoutException 调用超时。
     * @throws ServiceNotAvailableException 服务不可用。
     */
    template<typename ReqT, typename RspT>
    RspT call_method_sync(MethodId method_id, const ReqT& request,
                          std::uint32_t timeout_ms = 5000);

    /**
     * @brief 异步调用远端 Method。
     *
     * @tparam ReqT 请求类型，必须提供 serialize()。
     * @tparam RspT 响应类型，必须提供静态 deserialize()。
     * @param method_id Method 标识。
     * @param request 请求数据。
     * @param callback 返回状态与响应的回调。
     * @param timeout_ms 超时时间，单位毫秒。
     */
    template<typename ReqT, typename RspT>
    void call_method_async(MethodId method_id, const ReqT& request,
                           MethodReplyCallback callback,
                           std::uint32_t timeout_ms = 5000);

    /**
     * @brief 幂等的同步调用：超时时用同一个请求 key 重试，服务端按 key 去重。
     *
     * 需要两侧都启用 set_rpc_idempotency_enabled(true)；未启用时退化为单次调用。
     * 只在 TIMEOUT / NOT_AVAILABLE 时重试，其余错误立即上抛。
     */
    template<typename ReqT, typename RspT>
    RspT call_method_sync_idempotent(MethodId method_id, const ReqT& request,
                                     std::uint32_t timeout_ms = 5000,
                                     unsigned max_attempts = 2);

    // ---- 服务可用性 ----

    /// 查询服务当前是否可用
    bool is_available() const;

    /// 注册可用性变化回调
    void on_availability_change(AvailabilityCallback callback);

    // ---- 配置 ----

    ServiceId service_id() const { return m_config.service_id; }
    InstanceId instance_id() const { return m_config.instance_id; }

    void set_e2e_enabled(bool enabled) { m_e2e_enabled = enabled; }
    void set_fragmentation_enabled(bool enabled) { m_fragmentation_enabled = enabled; }
    void configure_connection_safety(bool heartbeat_enabled,
                                     std::uint32_t heartbeat_timeout_ms,
                                     bool reconnect_enabled,
                                     std::uint32_t initial_delay_ms,
                                     std::uint32_t max_delay_ms,
                                     std::uint32_t multiplier);

    /// 启用事件断点续收：必须与对端 Skeleton 一致（否则收不到序号头）
    void set_resume_enabled(bool enabled) { m_resume_enabled = enabled; }

    /// 启用同步 RPC 的"至多一次"：方法请求带 64 位 key，必须与对端一致
    void set_rpc_idempotency_enabled(bool enabled) { m_rpc_idempotency_enabled = enabled; }

    /// 对单个 Event 主动发起一次续收请求；返回服务端的回放结论
    safety::ResumeStatus request_resume(EventId event_id, std::uint32_t timeout_ms = 2000);

    /// 续收发现缺口时的通知回调；参数为服务端现存的最早序号
    void on_resume_gap(std::function<void(EventId, std::uint32_t)> callback);

private:
    // 对收到的 Event 字节执行分片重组与 E2E 校验
    void on_raw_event_received(EventId event_id, const std::vector<std::uint8_t>& data);
    void dispatch_complete_event(EventId event_id, const std::vector<std::uint8_t>& data);

    // 重连恢复后，对所有已订阅事件自动发起一次续收
    void resume_subscriptions_async();

    // 生成下一个请求 key（进程内单调）
    std::uint64_t next_request_key();

    // 启用至多一次时给请求加 8 字节 key 前缀；否则原样返回
    std::vector<std::uint8_t> frame_request(
        std::uint64_t key, const std::vector<std::uint8_t>& payload) const;

    std::shared_ptr<runtime::ICommunicationBackend> m_backend;
    ServiceConfig m_config;

    bool m_initialized{false};
    bool m_started{false};
    bool m_e2e_enabled{true};
    bool m_fragmentation_enabled{true};
    bool m_heartbeat_enabled{true};
    bool m_reconnect_enabled{true};
    std::uint32_t m_heartbeat_timeout_ms{3000};

    // 断点续收：已见过的最大序号，以及缺口通知
    bool m_resume_enabled{false};
    std::map<EventId, std::uint32_t> m_last_seq;
    std::function<void(EventId, std::uint32_t)> m_resume_gap_callback;

    // 同步 RPC 至多一次：请求 key 生成
    bool m_rpc_idempotency_enabled{false};
    std::atomic<std::uint64_t> m_request_key_counter{0};

    std::atomic<ServiceAvailability> m_availability{ServiceAvailability::UNKNOWN};

    // E2E 与分片重组
    std::uint8_t m_e2e_max_counter{14};
    std::map<EventId, std::unique_ptr<safety::E2EChecker>> m_e2e_checkers;
    std::map<EventId, std::unique_ptr<safety::MessageReassembler>> m_reassemblers;

    // 心跳监控与重连
    std::unique_ptr<safety::HeartbeatMonitor> m_heartbeat_monitor;
    std::unique_ptr<safety::ReconnectManager> m_reconnect_mgr;

    // 以 event_id 为键保存业务回调
    std::map<EventId, EventCallback> m_event_callbacks;
    std::map<EventId, EventCallback> m_transport_callbacks;

    std::mutex m_mutex;
};

// =========================================================================
//  模板实现
// =========================================================================

template<typename T>
void ServiceProxy::subscribe_event(EventId event_id,
                                   std::function<void(const T&)> callback,
                                   EventGroupId group_id) {
    // 将类型化回调包装为带反序列化的原始字节回调
    auto raw_cb = [cb = std::move(callback)](const std::vector<std::uint8_t>& data) {
        T value = T::deserialize(data);
        cb(value);
    };

    subscribe_event_raw(event_id, std::move(raw_cb), group_id);
}

template<typename ReqT, typename RspT>
RspT ServiceProxy::call_method_sync(MethodId method_id, const ReqT& request,
                                    std::uint32_t timeout_ms) {
    // 序列化请求；启用至多一次时在前面加一个本次调用专属的请求 key
    const std::vector<std::uint8_t> req_bytes = frame_request(
        next_request_key(), request.serialize());

    std::vector<std::uint8_t> rsp_bytes;
    ComRet ret = m_backend->call_method_sync(
        m_config.service_id, m_config.instance_id,
        method_id, req_bytes, rsp_bytes, timeout_ms);

    if (ret == ComRet::TIMEOUT) {
        throw TimeoutException("Method call " + std::to_string(method_id) + " timed out");
    }
    if (ret != ComRet::OK) {
        throw ServiceNotAvailableException("Method call " + std::to_string(method_id) +
                                           " failed: code=" + std::to_string(static_cast<int>(ret)));
    }

    // 反序列化响应
    return RspT::deserialize(rsp_bytes);
}

template<typename ReqT, typename RspT>
RspT ServiceProxy::call_method_sync_idempotent(MethodId method_id, const ReqT& request,
                                               std::uint32_t timeout_ms,
                                               unsigned max_attempts) {
    if (!m_rpc_idempotency_enabled) {
        ARA_LOG_WARN("PROXY", "Idempotent call requested while disabled; single attempt only");
        return call_method_sync<ReqT, RspT>(method_id, request, timeout_ms);
    }
    if (max_attempts == 0) max_attempts = 1;

    // 一次逻辑调用只用一个 key：重试复用，服务端才能把重试认成同一次调用。
    const std::uint64_t key = next_request_key();
    const std::vector<std::uint8_t> payload = request.serialize();

    ComRet last = ComRet::INTERNAL_ERR;
    for (unsigned attempt = 0; attempt < max_attempts; ++attempt) {
        std::vector<std::uint8_t> rsp_bytes;
        last = m_backend->call_method_sync(
            m_config.service_id, m_config.instance_id, method_id,
            frame_request(key, payload), rsp_bytes, timeout_ms);
        if (last == ComRet::OK) return RspT::deserialize(rsp_bytes);
        // 只有"没有确定答复"的情况才重试；其它错误立即上抛。
        if (last != ComRet::TIMEOUT && last != ComRet::NOT_AVAILABLE) break;
    }

    if (last == ComRet::TIMEOUT) {
        throw TimeoutException("Idempotent method call " + std::to_string(method_id) +
                               " timed out after " + std::to_string(max_attempts) + " attempt(s)");
    }
    throw ServiceNotAvailableException("Idempotent method call " + std::to_string(method_id) +
                                       " failed: code=" + std::to_string(static_cast<int>(last)));
}

template<typename ReqT, typename RspT>
void ServiceProxy::call_method_async(MethodId method_id, const ReqT& request,
                                     MethodReplyCallback callback,
                                     std::uint32_t timeout_ms) {
    const std::vector<std::uint8_t> req_bytes = frame_request(
        next_request_key(), request.serialize());

    m_backend->call_method_async(
        m_config.service_id, m_config.instance_id,
        method_id, req_bytes, std::move(callback), timeout_ms);
}

} // namespace com
} // namespace ara
