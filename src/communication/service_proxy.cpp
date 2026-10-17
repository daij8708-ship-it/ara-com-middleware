/**
 * @file    service_proxy.cpp
 * @brief   ServiceProxy 实现。
 */

#include "service_proxy.h"
#include "common/logger.h"

#include <algorithm>

namespace ara {
namespace com {

ServiceProxy::ServiceProxy(std::shared_ptr<runtime::ICommunicationBackend> backend,
                           ServiceConfig config,
                           E2EConfig e2e_config)
    : m_backend(std::move(backend))
    , m_config(std::move(config))
    , m_e2e_max_counter(e2e_config.max_counter)
{
    ARA_REQUIRE_NON_NULL(m_backend, "Backend must not be null");

    m_heartbeat_monitor = std::make_unique<safety::HeartbeatMonitor>();
    m_reconnect_mgr = std::make_unique<safety::ReconnectManager>();
}

ServiceProxy::~ServiceProxy() {
    stop();
}

bool ServiceProxy::init() {
    if (m_initialized) return true;

    // 配置重连回调
    m_reconnect_mgr->set_callbacks(
        // 重新发起服务发现请求
        [this]() -> bool {
            ARA_LOG_INFO("PROXY", "Attempting reconnection...");
            m_backend->request_service(
                m_config.service_id, m_config.instance_id,
                m_config.major_version, m_config.minor_version);
            // request_service 只表示发现请求已提交；只有 availability 回调
            // 收到 AVAILABLE 才能确认服务真正恢复。
            return false;
        },
        // 恢复后重新订阅 Event，并补收断连期间漏掉的事件
        [this]() {
            ARA_LOG_INFO("PROXY", "Reconnected — re-subscribing events");
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (auto& [eid, cb] : m_transport_callbacks) {
                    m_backend->subscribe_event(
                        m_config.service_id, m_config.instance_id,
                        eid, 1, cb);
                }
                for (auto& entry : m_e2e_checkers) entry.second->reset();
            }
            resume_subscriptions_async();
        });

    // 注册服务可用性回调
    m_backend->register_availability_callback(
        [this](ServiceId sid, InstanceId iid, ServiceAvailability avail) {
            if (sid == m_config.service_id && iid == m_config.instance_id) {
                m_availability.store(avail);
                if (avail == ServiceAvailability::AVAILABLE) {
                    ARA_LOG_INFO("PROXY", "Service available: " +
                                 std::to_string(sid) + "." + std::to_string(iid));
                    m_reconnect_mgr->on_connection_ok();
                } else {
                    ARA_LOG_WARN("PROXY", "Service unavailable: " +
                                 std::to_string(sid) + "." + std::to_string(iid));
                    if (m_reconnect_enabled) m_reconnect_mgr->on_connection_lost();
                }
            }
        });

    m_initialized = true;
    ARA_LOG_INFO("PROXY", "Initialized for service " +
                 std::to_string(m_config.service_id));
    return true;
}

bool ServiceProxy::start() {
    if (!m_initialized) {
        ARA_LOG_ERROR("PROXY", "Not initialized");
        return false;
    }

    if (m_started) return true;

    // 请求远端服务
    m_backend->request_service(m_config.service_id, m_config.instance_id,
                               m_config.major_version, m_config.minor_version);

    // 心跳使用保留 Event，不进入业务 Event 回调表
    if (m_heartbeat_enabled) m_backend->subscribe_event(m_config.service_id, m_config.instance_id,
                               0x8002, 1,
                               [this](const std::vector<std::uint8_t>& data) {
                                   if (data.size() == 2 && data[0] == 0x48 && data[1] == 0x42) {
                                       m_heartbeat_monitor->on_heartbeat_received();
                                   }
                               });

    // 启动心跳监控
    if (m_heartbeat_enabled) m_heartbeat_monitor->start(m_heartbeat_timeout_ms, [this]() {
        ARA_LOG_WARN("PROXY", "Heartbeat lost for service " +
                     std::to_string(m_config.service_id));
        m_availability.store(ServiceAvailability::UNAVAILABLE);
        if (m_reconnect_enabled) m_reconnect_mgr->on_connection_lost();
    });

    m_started = true;
    ARA_LOG_INFO("PROXY", "Started for service " +
                 std::to_string(m_config.service_id));
    return true;
}

void ServiceProxy::stop() {
    if (!m_started) return;

    m_heartbeat_monitor->stop();
    m_reconnect_mgr->stop();
    m_started = false;

    ARA_LOG_INFO("PROXY", "Stopped for service " +
                 std::to_string(m_config.service_id));
}

void ServiceProxy::subscribe_event_raw(EventId event_id,
                                       EventCallback callback,
                                       EventGroupId group_id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 每个 Event 独立维护检查器和重组器，避免状态互相干扰
    m_event_callbacks[event_id] = callback;
    m_e2e_checkers[event_id] = std::make_unique<safety::E2EChecker>(m_e2e_max_counter);
    auto reassembler = std::make_unique<safety::MessageReassembler>();
    reassembler->set_callback([this, event_id](const std::vector<std::uint8_t>& complete) {
        dispatch_complete_event(event_id, complete);
    });
    m_reassemblers[event_id] = std::move(reassembler);
    auto transport_callback = [this, event_id](const std::vector<std::uint8_t>& data) {
        on_raw_event_received(event_id, data);
    };
    m_transport_callbacks[event_id] = transport_callback;

    // 通过后端订阅
    m_backend->subscribe_event(
        m_config.service_id, m_config.instance_id,
        event_id, group_id, std::move(transport_callback));

    ARA_LOG_DEBUG("PROXY", "Subscribed to event_id=" + std::to_string(event_id));
}

bool ServiceProxy::is_available() const {
    return m_availability.load() == ServiceAvailability::AVAILABLE;
}

void ServiceProxy::on_availability_change(AvailabilityCallback callback) {
    m_backend->register_availability_callback(std::move(callback));
}

void ServiceProxy::configure_connection_safety(bool heartbeat_enabled,
                                                std::uint32_t heartbeat_timeout_ms,
                                                bool reconnect_enabled,
                                                std::uint32_t initial_delay_ms,
                                                std::uint32_t max_delay_ms,
                                                std::uint32_t multiplier) {
    m_heartbeat_enabled = heartbeat_enabled;
    m_reconnect_enabled = reconnect_enabled;
    m_heartbeat_timeout_ms = std::max<std::uint32_t>(1U, heartbeat_timeout_ms);
    m_reconnect_mgr->configure(initial_delay_ms, max_delay_ms, multiplier);
}

void ServiceProxy::on_raw_event_received(EventId event_id,
                                         const std::vector<std::uint8_t>& data) {
    // 检查心跳标记（event_id 0x8002，负载为 HB）
    if (data.size() == 2 && data[0] == 0x48 && data[1] == 0x42) {
        m_heartbeat_monitor->on_heartbeat_received();
        return;
    }

    if (!m_fragmentation_enabled) {
        dispatch_complete_event(event_id, data);
        return;
    }
    safety::MessageReassembler* reassembler = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_reassemblers.find(event_id);
        if (it != m_reassemblers.end()) reassembler = it->second.get();
    }
    if (reassembler) reassembler->process_fragment(data, m_config.service_id);
}

void ServiceProxy::dispatch_complete_event(EventId event_id,
                                            const std::vector<std::uint8_t>& data) {
    EventCallback callback;
    std::vector<std::uint8_t> payload = data;
    bool deliver = true;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_e2e_enabled) {
            auto checker = m_e2e_checkers.find(event_id);
            if (checker == m_e2e_checkers.end() ||
                checker->second->check(data, payload) != ComRet::OK) {
                ARA_LOG_WARN("PROXY", "E2E check failed for event " +
                             std::to_string(event_id));
                return;
            }
        }

        if (m_resume_enabled) {
            if (payload.size() < safety::RESUME_SEQ_HEADER_SIZE) {
                ARA_LOG_WARN("PROXY", "Sequenced event too short for event " +
                             std::to_string(event_id));
                return;
            }
            const std::uint32_t seq = safety::decode_seq(payload);
            payload.erase(payload.begin(),
                          payload.begin() + safety::RESUME_SEQ_HEADER_SIZE);
            auto last = m_last_seq.find(event_id);
            if (last != m_last_seq.end() && seq <= last->second) {
                // 回放与实时流可能重叠：序号不大于已见过的，说明是重复，丢弃。
                deliver = false;
            } else {
                m_last_seq[event_id] = seq;
            }
        }

        auto it = m_event_callbacks.find(event_id);
        if (it != m_event_callbacks.end()) callback = it->second;
    }
    if (deliver && callback) callback(payload);
}

void ServiceProxy::on_resume_gap(std::function<void(EventId, std::uint32_t)> callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_resume_gap_callback = std::move(callback);
}

safety::ResumeStatus ServiceProxy::request_resume(EventId event_id, std::uint32_t timeout_ms) {
    if (!m_resume_enabled) {
        ARA_LOG_WARN("PROXY", "Resume requested while disabled for event " +
                     std::to_string(event_id));
        return safety::ResumeStatus::GAP;
    }

    std::uint32_t last_seq = safety::RESUME_SEQ_UNKNOWN;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_last_seq.find(event_id);
        if (it != m_last_seq.end()) last_seq = it->second;
    }

    const auto request = safety::encode_resume_request(event_id, last_seq);
    std::vector<std::uint8_t> response;
    const ComRet ret = m_backend->call_method_sync(
        m_config.service_id, m_config.instance_id, safety::RESUME_METHOD_ID,
        request, response, timeout_ms);
    if (ret != ComRet::OK) {
        ARA_LOG_WARN("PROXY", "Resume request failed for event " +
                     std::to_string(event_id));
        return safety::ResumeStatus::GAP;
    }

    safety::ResumeStatus status = safety::ResumeStatus::GAP;
    std::uint16_t count = 0;
    std::uint32_t earliest = safety::RESUME_SEQ_UNKNOWN;
    if (!safety::decode_resume_response(response, status, count, earliest)) {
        ARA_LOG_WARN("PROXY", "Malformed resume response for event " +
                     std::to_string(event_id));
        return safety::ResumeStatus::GAP;
    }

    ARA_LOG_INFO("PROXY", "Resume event=" + std::to_string(event_id) +
                 " replayed=" + std::to_string(count) +
                 " status=" + std::to_string(static_cast<int>(status)));
    if (status == safety::ResumeStatus::GAP) {
        std::function<void(EventId, std::uint32_t)> cb;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cb = m_resume_gap_callback;
        }
        if (cb) cb(event_id, earliest);
    }
    return status;
}

std::uint64_t ServiceProxy::next_request_key() {
    return ++m_request_key_counter;
}

std::vector<std::uint8_t> ServiceProxy::frame_request(
    std::uint64_t key, const std::vector<std::uint8_t>& payload) const {
    if (!m_rpc_idempotency_enabled) {
        return payload;
    }
    std::vector<std::uint8_t> framed = safety::encode_request_key(key);
    framed.insert(framed.end(), payload.begin(), payload.end());
    return framed;
}

void ServiceProxy::resume_subscriptions_async() {
    if (!m_resume_enabled) return;

    std::vector<std::pair<EventId, std::uint32_t>> targets;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& entry : m_transport_callbacks) {
            auto last = m_last_seq.find(entry.first);
            targets.emplace_back(entry.first,
                                 last == m_last_seq.end() ? safety::RESUME_SEQ_UNKNOWN
                                                          : last->second);
        }
    }

    for (const auto& target : targets) {
        const auto request = safety::encode_resume_request(target.first, target.second);
        m_backend->call_method_async(
            m_config.service_id, m_config.instance_id, safety::RESUME_METHOD_ID,
            request,
            [this, event_id = target.first](ComRet status,
                                            const std::vector<std::uint8_t>& response) {
                if (status != ComRet::OK) return;
                safety::ResumeStatus resumed = safety::ResumeStatus::GAP;
                std::uint16_t count = 0;
                std::uint32_t earliest = safety::RESUME_SEQ_UNKNOWN;
                if (!safety::decode_resume_response(response, resumed, count, earliest)) return;
                ARA_LOG_INFO("PROXY", "Auto-resume event=" + std::to_string(event_id) +
                             " replayed=" + std::to_string(count));
                if (resumed == safety::ResumeStatus::GAP) {
                    std::function<void(EventId, std::uint32_t)> cb;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        cb = m_resume_gap_callback;
                    }
                    if (cb) cb(event_id, earliest);
                }
            },
            2000);
    }
}

} // namespace com
} // namespace ara
