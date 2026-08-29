/**
 * @file    service_skeleton.h
 * @brief   ServiceSkeleton：AP AUTOSAR ara::com 风格的服务端抽象。
 *
 * ServiceSkeleton 表示服务端点，封装 SOME/IP-SD 服务发布、Event 注册与发布、
 * Method 请求处理，以及 E2E 保护、分片和心跳机制。
 *
 * Usage:
 * @code
 *   ara::com::ServiceSkeleton skeleton(backend, service_config);
 *   skeleton.init();
 *   skeleton.register_event<MyData>(EVENT_ID, [](auto& data) { ... });
 *   skeleton.register_method<ReqT, RspT>(METHOD_ID, [](auto& req) { return rsp; });
 *   skeleton.start();
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

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ara {
namespace com {

/**
 * @class ServiceSkeleton
 * @brief 服务端点。
 *
 * 管理服务初始化、Event 发布、Method 处理和平稳停止，并透明集成 E2E 保护与分片。
 */
class ServiceSkeleton {
public:
    /**
     * @brief 构造 ServiceSkeleton。
     * @param backend 通信后端，可使用 vsomeip 或测试用 NullBackend。
     * @param config 从 JSON 读取的服务配置。
     * @param e2e_config E2E 保护配置。
     */
    ServiceSkeleton(std::shared_ptr<runtime::ICommunicationBackend> backend,
                    ServiceConfig config,
                    E2EConfig e2e_config = {});

    ~ServiceSkeleton();

    ServiceSkeleton(const ServiceSkeleton&) = delete;
    ServiceSkeleton& operator=(const ServiceSkeleton&) = delete;

    // ---- 生命周期 ----

    /// 初始化 Skeleton，但暂不发布服务
    bool init();

    /// 开始发布服务
    bool start();

    /// 停止服务
    void stop();

    // ---- Event 注册与发布 ----

    /**
     * @brief 注册 Event 发布器。
     *
     * @tparam T 待序列化类型，必须提供返回字节数组的 serialize() 方法。
     * @param event_id 服务内 Event 标识。
     * @param serializer 将 T 转换为字节的函数，默认调用 T::serialize。
     */
    template<typename T>
    void register_event(EventId event_id,
                        std::function<std::vector<std::uint8_t>(const T&)> serializer = nullptr);

    /**
     * @brief 向所有订阅客户端发布 Event。
     * @tparam T 数据类型，必须提供 serialize() 或已注册序列化函数。
     * @param event_id Event 标识。
     * @param data 待发布数据。
     */
    template<typename T>
    bool publish_event(EventId event_id, const T& data);

    /// 以原始字节形式发布 Event
    bool publish_event_raw(EventId event_id, const std::vector<std::uint8_t>& data);

    // ---- Method 注册 ----

    /**
     * @brief 注册同步 Method 处理函数。
     *
     * @tparam ReqT 请求类型，必须提供静态 deserialize()。
     * @tparam RspT 响应类型，必须提供 serialize()。
     * @param method_id Method 标识。
     * @param handler 处理请求并返回响应的函数。
     */
    template<typename ReqT, typename RspT>
    void register_method(MethodId method_id,
                         std::function<RspT(const ReqT&)> handler);

    // ---- 配置访问 ----

    ServiceId service_id() const { return m_config.service_id; }
    InstanceId instance_id() const { return m_config.instance_id; }

    /// 启用或关闭 Event 的 E2E 保护
    void set_e2e_enabled(bool enabled) { m_e2e_enabled = enabled; }

    /// 启用或关闭消息分片
    void set_fragmentation_enabled(bool enabled) { m_fragmentation_enabled = enabled; }

    /// 设置分片 MTU
    void set_mtu(std::uint16_t mtu);

    /// 启用心跳
    void enable_heartbeat(std::uint32_t interval_ms);

private:
    // 负责序列化与反序列化的内部 Method 处理入口
    void register_method_internal(MethodId method_id, MethodHandler handler);

    // 发送前执行 E2E 保护和分片
    std::vector<std::vector<std::uint8_t>> prepare_outgoing(const std::vector<std::uint8_t>& raw_data);

    std::shared_ptr<runtime::ICommunicationBackend> m_backend;
    ServiceConfig m_config;
    E2EConfig m_e2e_config;

    bool m_initialized{false};
    bool m_started{false};
    bool m_e2e_enabled{true};
    bool m_fragmentation_enabled{true};
    // 以 event_id 为键保存序列化函数
    std::map<EventId, std::function<std::vector<std::uint8_t>(const void*)>> m_event_serializers;

    std::unique_ptr<safety::E2EProtector> m_e2e_protector;
    std::unique_ptr<safety::MessageFragmenter> m_fragmenter;
    std::unique_ptr<safety::HeartbeatProvider> m_heartbeat;

    std::mutex m_mutex;
};

// =========================================================================
//  模板实现（必须放在头文件中）
// =========================================================================

template<typename T>
void ServiceSkeleton::register_event(EventId event_id,
                                     std::function<std::vector<std::uint8_t>(const T&)> serializer) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 保存类型擦除后的序列化函数
    m_event_serializers[event_id] = [ser = std::move(serializer)](const void* ptr) -> std::vector<std::uint8_t> {
        const T* typed = static_cast<const T*>(ptr);
        if (ser) {
            return ser(*typed);
        }
        // 默认调用 T::serialize() 成员函数
        return typed->serialize();
    };
    m_backend->register_event(m_config.service_id, m_config.instance_id, event_id, 1);
}

template<typename T>
bool ServiceSkeleton::publish_event(EventId event_id, const T& data) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_event_serializers.find(event_id);
    if (it == m_event_serializers.end()) {
        ARA_LOG_ERROR("SKELETON", "No serializer registered for event_id=" +
                      std::to_string(event_id));
        return false;
    }

    // Serialize
    std::vector<std::uint8_t> serialized = it->second(&data);

    // Publish raw bytes
    return publish_event_raw(event_id, serialized);
}

template<typename ReqT, typename RspT>
void ServiceSkeleton::register_method(MethodId method_id,
                                      std::function<RspT(const ReqT&)> handler) {
    // 将类型化处理函数包装为原始字节处理函数
    MethodHandler raw_handler = [handler = std::move(handler)](
        const std::vector<std::uint8_t>& request_bytes) -> std::vector<std::uint8_t> {

        // 反序列化请求
        ReqT request = ReqT::deserialize(request_bytes);

        // 调用业务处理函数
        RspT response = handler(request);

        // 序列化响应
        return response.serialize();
    };

    register_method_internal(method_id, std::move(raw_handler));
}

} // namespace com
} // namespace ara
