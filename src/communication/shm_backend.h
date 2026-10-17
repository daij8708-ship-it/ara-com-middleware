/**
 * @file    shm_backend.h
 * @brief   基于同机共享内存的 ICommunicationBackend 实现。
 *
 * 服务端创建控制通道，客户端注册后获得一条独立的双向共享内存通道；
 * Method 请求按方法号路由到处理函数，Event 只发送给已订阅的客户端。
 */
#pragma once

#include "runtime.h"

#if defined(ARA_COM_USE_POSIX_SHM)

#include <memory>

namespace ara {
namespace com {
namespace runtime {

/**
 * @class ShmBackend
 * @brief 同机多客户端共享内存后端。
 *
 * 数据面使用 POSIX 共享内存环形队列，控制面只负责客户端注册和订阅。
 * 同一服务端最多支持 SHM_MAX_CLIENTS 个并发客户端。
 */
class ShmBackend : public ICommunicationBackend {
public:
    ShmBackend();
    ~ShmBackend() override;

    ShmBackend(const ShmBackend&) = delete;
    ShmBackend& operator=(const ShmBackend&) = delete;

    bool init(const ServiceConfig& config) override;
    bool start() override;
    void stop() override;

    bool offer_service(ServiceId sid, InstanceId iid,
                       MajorVersion major, MinorVersion minor) override;
    void stop_offer_service(ServiceId sid, InstanceId iid) override;
    bool register_method_handler(ServiceId sid, InstanceId iid,
                                 MethodId mid, MethodHandler handler) override;
    bool register_event(ServiceId sid, InstanceId iid,
                        EventId eid, EventGroupId gid) override;
    bool send_event(ServiceId sid, InstanceId iid,
                    EventId eid, const std::vector<std::uint8_t>& data) override;

    bool request_service(ServiceId sid, InstanceId iid,
                         MajorVersion major, MinorVersion minor) override;
    bool subscribe_event(ServiceId sid, InstanceId iid,
                         EventId eid, EventGroupId gid,
                         EventCallback callback) override;
    ComRet call_method_sync(ServiceId sid, InstanceId iid,
                            MethodId mid,
                            const std::vector<std::uint8_t>& request,
                            std::vector<std::uint8_t>& response,
                            std::uint32_t timeout_ms = 5000) override;
    bool call_method_async(ServiceId sid, InstanceId iid,
                           MethodId mid,
                           const std::vector<std::uint8_t>& request,
                           MethodReplyCallback callback,
                           std::uint32_t timeout_ms = 5000) override;

    void register_availability_callback(AvailabilityCallback callback) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace runtime
} // namespace com
} // namespace ara

#endif // ARA_COM_USE_POSIX_SHM
