/**
 * @file    runtime.h
 * @brief   通信运行时：抽象后端接口与 vsomeip 适配器。
 *
 * ICommunicationBackend 用于抽象底层传输；VSomeIPBackend 基于 vsomeip 实现，
 * NullBackend 用于在未安装 vsomeip 时进行确定性测试。
 *
 * ServiceSkeleton 和 ServiceProxy 仅依赖抽象接口，因此可以替换传输实现。
 *
 */
#pragma once

#include "common/types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ara {
namespace com {
namespace runtime {

// =========================================================================
//  通信后端抽象
// =========================================================================

/**
 * @class ICommunicationBackend
 * @brief 底层传输的抽象接口。
 *
 * 实现类负责具体消息传输，上层 ServiceSkeleton/ServiceProxy 只面向本接口编程。
 */
class ICommunicationBackend {
public:
    virtual ~ICommunicationBackend() = default;

    // ---- 生命周期 ----

    /// 初始化后端，启动阶段调用一次
    virtual bool init(const ServiceConfig& config) = 0;

    /// 启动通信，开始监听或服务发现
    virtual bool start() = 0;

    /// 平稳停止通信
    virtual void stop() = 0;

    // ---- 服务端（Skeleton）----

    /// 发布服务，使其可被发现
    virtual bool offer_service(ServiceId sid, InstanceId iid,
                               MajorVersion major, MinorVersion minor) = 0;

    /// 停止发布服务
    virtual void stop_offer_service(ServiceId sid, InstanceId iid) = 0;

    /// 注册 Method 请求处理函数
    virtual bool register_method_handler(ServiceId sid, InstanceId iid,
                                         MethodId mid, MethodHandler handler) = 0;

    /// 在发布服务前声明 Event
    virtual bool register_event(ServiceId sid, InstanceId iid,
                                EventId eid, EventGroupId gid) = 0;

    /// 向所有订阅客户端发送 Event
    virtual bool send_event(ServiceId sid, InstanceId iid,
                            EventId eid, const std::vector<std::uint8_t>& data) = 0;

    // ---- 客户端（Proxy）----

    /// 请求远端服务，触发 SOME/IP-SD FindService
    virtual bool request_service(ServiceId sid, InstanceId iid,
                                 MajorVersion major, MinorVersion minor) = 0;

    /// 订阅远端 Event
    virtual bool subscribe_event(ServiceId sid, InstanceId iid,
                                 EventId eid, EventGroupId gid,
                                 EventCallback callback) = 0;

    /// 同步调用远端 Method（阻塞）
    virtual ComRet call_method_sync(ServiceId sid, InstanceId iid,
                                    MethodId mid,
                                    const std::vector<std::uint8_t>& request,
                                    std::vector<std::uint8_t>& response,
                                    std::uint32_t timeout_ms = 5000) = 0;

    /// 异步调用远端 Method
    virtual bool call_method_async(ServiceId sid, InstanceId iid,
                                   MethodId mid,
                                   const std::vector<std::uint8_t>& request,
                                   MethodReplyCallback callback,
                                   std::uint32_t timeout_ms = 5000) = 0;

    // ---- 可用性 ----

    /// 注册服务可用性变化回调
    virtual void register_availability_callback(AvailabilityCallback callback) = 0;
};

// =========================================================================
//  后端工厂
// =========================================================================

/// 支持的后端类型
enum class BackendType {
    VSOMEIP,    ///< 使用 vsomeip，需要 libvsomeip3
    SHM,        ///< 同机共享内存，支持多客户端
    DDS,        ///< 使用 Fast DDS，基于 RTPS/UDP
    NULL_BACKEND ///< 用于单元测试的进程内后端
};

/// 创建指定类型的后端实例
std::shared_ptr<ICommunicationBackend> create_backend(BackendType type);

} // namespace runtime
} // namespace com
} // namespace ara
