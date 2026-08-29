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
#include "runtime.h"
#include "safety/e2e.h"
#include "safety/fragmentation.h"
#include "safety/heartbeat.h"
#include "safety/reconnect.h"

#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>

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

private:
    // 对收到的 Event 字节执行分片重组与 E2E 校验
    void on_raw_event_received(EventId event_id, const std::vector<std::uint8_t>& data);
    void dispatch_complete_event(EventId event_id, const std::vector<std::uint8_t>& data);

    std::shared_ptr<runtime::ICommunicationBackend> m_backend;
    ServiceConfig m_config;

    bool m_initialized{false};
    bool m_started{false};
    bool m_e2e_enabled{true};
    bool m_fragmentation_enabled{true};
    bool m_heartbeat_enabled{true};
    bool m_reconnect_enabled{true};
    std::uint32_t m_heartbeat_timeout_ms{3000};

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
    // 序列化请求
    std::vector<std::uint8_t> req_bytes = request.serialize();

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
void ServiceProxy::call_method_async(MethodId method_id, const ReqT& request,
                                     MethodReplyCallback callback,
                                     std::uint32_t timeout_ms) {
    std::vector<std::uint8_t> req_bytes = request.serialize();

    m_backend->call_method_async(
        m_config.service_id, m_config.instance_id,
        method_id, req_bytes, std::move(callback), timeout_ms);
}

} // namespace com
} // namespace ara
