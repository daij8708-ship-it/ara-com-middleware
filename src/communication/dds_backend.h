#pragma once

#include "runtime.h"

#if defined(ARA_COM_USE_DDS)

namespace ara::com::runtime {

struct DdsBackendOptions {
    std::uint32_t domain_id{0};
};

class DdsBackend final : public ICommunicationBackend {
public:
    static constexpr std::size_t MAX_PAYLOAD_SIZE = 64 * 1024;
    static constexpr std::size_t MAX_PENDING_CALLS = 128;

    explicit DdsBackend(DdsBackendOptions options = {});
    ~DdsBackend() override;
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
    bool send_event(ServiceId sid, InstanceId iid, EventId eid,
                    const std::vector<std::uint8_t>& data) override;
    bool request_service(ServiceId sid, InstanceId iid,
                         MajorVersion major, MinorVersion minor) override;
    bool subscribe_event(ServiceId sid, InstanceId iid, EventId eid,
                         EventGroupId gid, EventCallback callback) override;
    ComRet call_method_sync(ServiceId sid, InstanceId iid, MethodId mid,
                            const std::vector<std::uint8_t>& request,
                            std::vector<std::uint8_t>& response,
                            std::uint32_t timeout_ms = 5000) override;
    bool call_method_async(ServiceId sid, InstanceId iid, MethodId mid,
                           const std::vector<std::uint8_t>& request,
                           MethodReplyCallback callback,
                           std::uint32_t timeout_ms = 5000) override;
    void register_availability_callback(AvailabilityCallback callback) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
#endif
