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

    // 断点续收开启时，注册保留的续收 Method
    if (m_resume_enabled) {
        register_method_internal(safety::RESUME_METHOD_ID,
            [this](const std::vector<std::uint8_t>& request) {
                return handle_resume_request(request);
            });
        ARA_LOG_INFO("SKELETON", "Resume enabled: buffer_capacity=" +
                     std::to_string(m_resume_buffer_capacity));
    }

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

    // 启用至多一次时，在原始字节处理函数外面包一层按键去重：
    // 先剥掉 8 字节请求 key，命中窗口直接回缓存，未命中才执行用户 handler。
    // 保留的控制 Method（续收）走自己的线格式，不能被 key 前缀误伤。
    if (m_rpc_idempotency_enabled && method_id != safety::RESUME_METHOD_ID) {
        MethodHandler inner = std::move(handler);
        handler = [this, method_id, inner](const std::vector<std::uint8_t>& request)
            -> std::vector<std::uint8_t> {
            if (request.size() < safety::REQUEST_KEY_SIZE) {
                ARA_LOG_WARN("SKELETON", "Idempotent request too short for method " +
                             std::to_string(method_id));
                return {};
            }
            std::vector<std::uint8_t> payload(
                request.begin() + safety::REQUEST_KEY_SIZE, request.end());
            const std::uint64_t key = safety::decode_request_key(request);

            {
                std::lock_guard<std::mutex> dedup_lock(m_idempotency_mutex);
                auto& window = m_idempotency_windows[method_id];
                window.set_capacity(m_idempotency_window_size);
                std::vector<std::uint8_t> cached;
                if (window.lookup(key, cached)) {
                    ARA_LOG_DEBUG("SKELETON", "Idempotency hit: method=" +
                                  std::to_string(method_id) +
                                  " key=" + std::to_string(key));
                    return cached;
                }
            }

            // handler 是用户代码，必须在锁外执行。
            std::vector<std::uint8_t> response = inner(payload);

            {
                std::lock_guard<std::mutex> dedup_lock(m_idempotency_mutex);
                m_idempotency_windows[method_id].record(key, response);
            }
            return response;
        };
    }

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

    if (!m_resume_enabled) {
        return emit_event(event_id, data, /*with_seq=*/false, 0);
    }

    // 分配序号并按需入缓存；序号与缓存受 m_resume_mutex 保护。
    std::uint32_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(m_resume_mutex);
        seq = m_next_seq[event_id]++;
        if (m_resume_buffer_capacity > 0) {
            auto& buffer = m_resume_buffers[event_id];
            buffer.set_capacity(m_resume_buffer_capacity);
            buffer.push(seq, data);
        }
    }
    return emit_event(event_id, data, /*with_seq=*/true, seq);
}

bool ServiceSkeleton::emit_event(EventId event_id, const std::vector<std::uint8_t>& business,
                                 bool with_seq, std::uint32_t seq) {
    std::vector<std::uint8_t> framed;
    if (with_seq) {
        // 序号头放在 E2E 之前，因此 CRC 覆盖序号，序号本身也被保护。
        framed = safety::encode_seq(seq);
        framed.insert(framed.end(), business.begin(), business.end());
    } else {
        framed = business;
    }

    // E2E 保护器和分片器都是有状态的，发布与回放必须串行使用。
    // 但出锁之后再发：send_event 会同步触发用户回调，持锁调用会重入死锁。
    std::vector<std::vector<std::uint8_t>> fragments;
    {
        std::lock_guard<std::mutex> lock(m_emit_mutex);
        fragments = prepare_outgoing(framed);
    }

    for (const auto& frag : fragments) {
        if (!m_backend->send_event(m_config.service_id, m_config.instance_id,
                                    event_id, frag)) {
            ARA_LOG_ERROR("SKELETON", "Failed to send event fragment");
            return false;
        }
    }

    return true;
}

std::vector<std::uint8_t> ServiceSkeleton::handle_resume_request(
    const std::vector<std::uint8_t>& request) {
    EventId event_id = 0;
    std::uint32_t last_seq = safety::RESUME_SEQ_UNKNOWN;
    if (!safety::decode_resume_request(request, event_id, last_seq)) {
        ARA_LOG_WARN("SKELETON", "Malformed resume request");
        return safety::encode_resume_response(safety::ResumeStatus::GAP, 0,
                                              safety::RESUME_SEQ_UNKNOWN);
    }

    std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> replay;
    std::uint32_t earliest = safety::RESUME_SEQ_UNKNOWN;
    bool gap = false;
    {
        std::lock_guard<std::mutex> lock(m_resume_mutex);
        auto it = m_resume_buffers.find(event_id);
        if (it == m_resume_buffers.end()) {
            // 没有任何缓存：只有客户端声称有历史时才算缺口。
            gap = (last_seq != safety::RESUME_SEQ_UNKNOWN);
        } else {
            gap = it->second.has_gap(last_seq);
            earliest = it->second.earliest();
            replay = it->second.since(last_seq);
        }
    }

    // 回放时重做一次 E2E（计数器是新的），但保留原序号。
    for (const auto& item : replay) {
        emit_event(event_id, item.second, /*with_seq=*/true, item.first);
    }

    const auto count = static_cast<std::uint16_t>(
        replay.size() > 0xFFFFu ? 0xFFFFu : replay.size());
    ARA_LOG_INFO("SKELETON", "Resume event=" + std::to_string(event_id) +
                 " last_seq=" + std::to_string(last_seq) +
                 " replayed=" + std::to_string(count) +
                 (gap ? " gap=1" : " gap=0"));
    return safety::encode_resume_response(
        gap ? safety::ResumeStatus::GAP : safety::ResumeStatus::OK, count, earliest);
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
