/**
 * @file    service_skeleton.cpp
 * @brief   ServiceSkeleton 实现。
 */

#include "service_skeleton.h"
#include "common/logger.h"

namespace ara {
namespace com {

ServiceSkeleton::ServiceSkeleton(std::shared_ptr<runtime::ICommunicationBackend> backend,
                                 ServiceConfig config,
                                 E2EConfig e2e_config)
    : m_backend(std::move(backend))
    , m_config(std::move(config))
    , m_e2e_config(e2e_config)
{
    ARA_REQUIRE_NON_NULL(m_backend, "Backend must not be null");

    m_e2e_protector = std::make_unique<safety::E2EProtector>(e2e_config.max_counter);
    m_fragmenter = std::make_unique<safety::MessageFragmenter>(safety::DEFAULT_MTU);
    m_heartbeat = std::make_unique<safety::HeartbeatProvider>();
}

ServiceSkeleton::~ServiceSkeleton() {
    stop();
}

bool ServiceSkeleton::init() {
    if (m_initialized) {
        ARA_LOG_WARN("SKELETON", "Already initialized");
        return true;
    }

    ARA_LOG_INFO("SKELETON", "Initializing service " +
                 std::to_string(m_config.service_id) + "." +
                 std::to_string(m_config.instance_id));

    m_initialized = true;
    return true;
}

bool ServiceSkeleton::start() {
    if (!m_initialized) {
        ARA_LOG_ERROR("SKELETON", "Not initialized — call init() first");
        return false;
    }

    if (m_started) {
        ARA_LOG_WARN("SKELETON", "Already started");
        return true;
    }

    // 发布服务前注册保留心跳 Event
    m_backend->register_event(m_config.service_id, m_config.instance_id, 0x8002, 1);

    // 通过后端发布服务
    if (!m_backend->offer_service(m_config.service_id, m_config.instance_id,
                                   m_config.major_version, m_config.minor_version)) {
        ARA_LOG_ERROR("SKELETON", "Failed to offer service");
        return false;
    }

    m_started = true;

    ARA_LOG_INFO("SKELETON", "Service started: " +
                 std::to_string(m_config.service_id) + "." +
                 std::to_string(m_config.instance_id) +
                 " @ " + m_config.ip_address + ":" + std::to_string(m_config.port));

    return true;
}

void ServiceSkeleton::stop() {
    if (!m_started) return;

    m_heartbeat->stop();

    m_backend->stop_offer_service(m_config.service_id, m_config.instance_id);
    m_started = false;

    ARA_LOG_INFO("SKELETON", "Service stopped: " +
                 std::to_string(m_config.service_id));
}

void ServiceSkeleton::register_method_internal(MethodId method_id, MethodHandler handler) {
    std::lock_guard<std::mutex> lock(m_mutex);

    m_backend->register_method_handler(
        m_config.service_id, m_config.instance_id, method_id, std::move(handler));

    ARA_LOG_DEBUG("SKELETON", "Registered method handler for method_id=" +
                  std::to_string(method_id));
}

bool ServiceSkeleton::publish_event_raw(EventId event_id, const std::vector<std::uint8_t>& data) {
    if (!m_started) {
        ARA_LOG_WARN("SKELETON", "Cannot publish — service not started");
        return false;
    }

    auto fragments = prepare_outgoing(data);

    for (const auto& frag : fragments) {
        if (!m_backend->send_event(m_config.service_id, m_config.instance_id,
                                    event_id, frag)) {
            ARA_LOG_ERROR("SKELETON", "Failed to send event fragment");
            return false;
        }
    }

    return true;
}

std::vector<std::vector<std::uint8_t>> ServiceSkeleton::prepare_outgoing(
    const std::vector<std::uint8_t>& raw_data) {

    std::vector<std::uint8_t> protected_data;

    // 第一步：添加 E2E 保护
    if (m_e2e_enabled) {
        protected_data = m_e2e_protector->protect(raw_data);
    } else {
        protected_data = raw_data;
    }

    // 第二步：启用分片时，单片与多片消息都携带统一分片头
    if (m_fragmentation_enabled) {
        return m_fragmenter->fragment(protected_data);
    }

    // 不启用分片时直接发送
    return {protected_data};
}

void ServiceSkeleton::set_mtu(std::uint16_t mtu) {
    m_fragmenter = std::make_unique<safety::MessageFragmenter>(mtu);
}

void ServiceSkeleton::enable_heartbeat(std::uint32_t interval_ms) {
    m_heartbeat->start(interval_ms, [this]() {
        // 发送最小心跳 Event，0x8002 为保留标识
        std::vector<std::uint8_t> hb_payload = {0x48, 0x42};  // "HB"
        m_backend->send_event(m_config.service_id, m_config.instance_id,
                              0x8002, hb_payload);
    });
}

} // namespace com
} // namespace ara
