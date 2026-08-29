/**
 * @file    runtime.cpp
 * @brief   通信后端实现。
 *
 * 提供用于单元测试的 NullBackend，以及按构建选项启用的 VSomeIPBackend。
 */

#include "runtime.h"
#include "common/logger.h"
#include "common/exception.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#ifdef ARA_COM_USE_VSOMEIP
#include <vsomeip/vsomeip.hpp>
#endif

namespace ara {
namespace com {
namespace runtime {

// =========================================================================
//  NullBackend：测试用进程内实现
// =========================================================================

class NullBackend : public ICommunicationBackend {
public:
    bool init(const ServiceConfig& config) override {
        m_config = config;
        ARA_LOG_INFO("NULL_BACKEND", "Initialized (service_id=" +
                     std::to_string(config.service_id) + " role=" + config.role + ")");
        return true;
    }

    bool start() override {
        ARA_LOG_INFO("NULL_BACKEND", "Started");
        return true;
    }

    void stop() override {
        ARA_LOG_INFO("NULL_BACKEND", "Stopped");
    }

    bool offer_service(ServiceId sid, InstanceId iid,
                       MajorVersion major, MinorVersion minor) override {
        ARA_LOG_DEBUG("NULL_BACKEND", "Offer service: " +
                      std::to_string(sid) + "." + std::to_string(iid));
        return true;
    }

    void stop_offer_service(ServiceId sid, InstanceId iid) override {
        ARA_LOG_DEBUG("NULL_BACKEND", "Stop offer service: " +
                      std::to_string(sid) + "." + std::to_string(iid));
    }

    bool register_method_handler(ServiceId sid, InstanceId iid,
                                 MethodId mid, MethodHandler handler) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_method_handlers[std::make_tuple(sid, iid, mid)] = std::move(handler);
        ARA_LOG_DEBUG("NULL_BACKEND", "Registered method handler: " +
                      std::to_string(sid) + "." + std::to_string(iid) +
                      " method=" + std::to_string(mid));
        return true;
    }

    bool register_event(ServiceId, InstanceId, EventId, EventGroupId) override {
        return true;
    }

    bool send_event(ServiceId sid, InstanceId iid,
                    EventId eid, const std::vector<std::uint8_t>& data) override {
        ARA_LOG_DEBUG("NULL_BACKEND", "Send event: " +
                      std::to_string(sid) + "." + std::to_string(iid) +
                      " event=" + std::to_string(eid) +
                      " size=" + std::to_string(data.size()));
        // NullBackend 直接向本地订阅者分发
        std::vector<EventCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_event_subscribers.find(std::make_tuple(sid, iid, eid));
            if (it != m_event_subscribers.end()) callbacks = it->second;
        }
        // 调用用户代码前释放后端互斥锁
        for (auto& cb : callbacks) {
            if (cb) cb(data);
        }
        return true;
    }

    bool request_service(ServiceId sid, InstanceId iid,
                         MajorVersion major, MinorVersion minor) override {
        ARA_LOG_DEBUG("NULL_BACKEND", "Request service: " +
                      std::to_string(sid) + "." + std::to_string(iid));
        // NullBackend 模拟服务立即可用
        std::vector<AvailabilityCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            callbacks = m_availability_callbacks;
        }
        for (auto& callback : callbacks) {
            if (callback) callback(sid, iid, ServiceAvailability::AVAILABLE);
        }
        return true;
    }

    bool subscribe_event(ServiceId sid, InstanceId iid,
                         EventId eid, EventGroupId gid,
                         EventCallback callback) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto key = std::make_tuple(sid, iid, eid);
        m_event_subscribers[key].push_back(std::move(callback));
        ARA_LOG_DEBUG("NULL_BACKEND", "Subscribed to event: " +
                      std::to_string(sid) + "." + std::to_string(iid) +
                      " event=" + std::to_string(eid));
        return true;
    }

    ComRet call_method_sync(ServiceId sid, InstanceId iid,
                            MethodId mid,
                            const std::vector<std::uint8_t>& request,
                            std::vector<std::uint8_t>& response,
                            std::uint32_t timeout_ms) override {
        MethodHandler handler;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_method_handlers.find(std::make_tuple(sid, iid, mid));
            if (it != m_method_handlers.end()) handler = it->second;
        }
        if (handler) {
            response = handler(request);
            return ComRet::OK;
        }
        return ComRet::NOT_AVAILABLE;
    }

    bool call_method_async(ServiceId sid, InstanceId iid,
                           MethodId mid,
                           const std::vector<std::uint8_t>& request,
                           MethodReplyCallback callback,
                           std::uint32_t timeout_ms) override {
        // NullBackend 同步执行后立即调用回调
        std::vector<std::uint8_t> response;
        ComRet ret = call_method_sync(sid, iid, mid, request, response, timeout_ms);
        if (callback) {
            callback(ret, response);
        }
        return ret == ComRet::OK;
    }

    void register_availability_callback(AvailabilityCallback callback) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_availability_callbacks.push_back(std::move(callback));
    }

private:
    ServiceConfig m_config;
    std::vector<AvailabilityCallback> m_availability_callbacks;
    std::mutex m_mutex;

    // Key: (service_id, instance_id, method_id)
    std::map<std::tuple<ServiceId, InstanceId, MethodId>, MethodHandler> m_method_handlers;

    // Key: (service_id, instance_id, event_id)
    std::map<std::tuple<ServiceId, InstanceId, EventId>, std::vector<EventCallback>> m_event_subscribers;
};

// =========================================================================
//  VSomeIPBackend：真实 vsomeip 适配（条件编译）
// =========================================================================

#ifdef ARA_COM_USE_VSOMEIP

class VSomeIPBackend : public ICommunicationBackend {
public:
    ~VSomeIPBackend() override { stop(); }

    bool init(const ServiceConfig& config) override {
        m_config = config;
        const std::string app_name = "ara_com_" + config.role + "_" +
                                     std::to_string(config.service_id);
        m_app = vsomeip::runtime::get()->create_application(app_name);

        if (!m_app) {
            ARA_LOG_ERROR("VSOMEIP", "Failed to create vsomeip application");
            return false;
        }

        // 注册状态回调
        m_app->register_state_handler([this](vsomeip::state_type_e state) {
            ARA_LOG_INFO("VSOMEIP", "State changed to " + std::to_string(static_cast<int>(state)));
            if (state == vsomeip::state_type_e::ST_REGISTERED) {
                on_registered();
            }
        });

        // 注册 Method 请求处理入口
        m_app->register_message_handler(
            vsomeip::ANY_SERVICE, vsomeip::ANY_INSTANCE, vsomeip::ANY_METHOD,
            [this](const std::shared_ptr<vsomeip::message>& msg) {
                on_message(msg);
            });

        // 注册服务可用性回调
        m_app->register_availability_handler(
            vsomeip::ANY_SERVICE, vsomeip::ANY_INSTANCE,
            [this](vsomeip::service_t sid, vsomeip::instance_t iid, bool available) {
                std::vector<AvailabilityCallback> callbacks;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const auto service_key = encode_key(sid, iid, 0);
                    if (available) m_available_services.insert(service_key);
                    else m_available_services.erase(service_key);
                    callbacks = m_availability_callbacks;
                }
                for (auto& callback : callbacks) if (callback) {
                    callback(static_cast<ServiceId>(sid), static_cast<InstanceId>(iid),
                             available ? ServiceAvailability::AVAILABLE
                                       : ServiceAvailability::UNAVAILABLE);
                }
                if (available) resubscribe(sid, iid);
            });

        return m_app->init();
    }

    bool start() override {
        if (!m_app || m_running.exchange(true)) return m_app != nullptr;
        // application::start() 持有事件循环并阻塞
        m_io_thread = std::thread([this]() { m_app->start(); });
        m_timeout_thread = std::thread([this]() { timeout_loop(); });
        std::unique_lock<std::mutex> lock(m_state_mutex);
        return m_state_cv.wait_for(lock, std::chrono::seconds(2),
                                   [this]() { return m_registered; });
    }

    void stop() override {
        if (!m_running.exchange(false)) return;
        fail_all_pending(ComRet::NOT_AVAILABLE);
        m_timeout_cv.notify_all();
        if (m_app) m_app->stop();
        if (m_io_thread.joinable()) m_io_thread.join();
        if (m_timeout_thread.joinable()) m_timeout_thread.join();
    }

    bool offer_service(ServiceId sid, InstanceId iid,
                       MajorVersion major, MinorVersion minor) override {
        if (m_app) {
            m_app->offer_service(sid, iid, major, minor);
            return true;
        }
        return false;
    }

    void stop_offer_service(ServiceId sid, InstanceId iid) override {
        if (m_app) {
            m_app->stop_offer_service(sid, iid);
        }
    }

    bool register_method_handler(ServiceId sid, InstanceId iid,
                                 MethodId mid, MethodHandler handler) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto key = encode_key(sid, iid, mid);
        m_method_handlers[key] = std::move(handler);
        return true;
    }

    bool register_event(ServiceId sid, InstanceId iid,
                        EventId eid, EventGroupId gid) override {
        if (!m_app) return false;
        const auto key = encode_key(sid, iid, eid);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_offered_events.insert(key).second) {
            m_app->offer_event(sid, iid, eid, {gid});
            m_registered_events.push_back({sid, iid, eid, gid});
        }
        return true;
    }

    bool send_event(ServiceId sid, InstanceId iid,
                    EventId eid, const std::vector<std::uint8_t>& data) override {
        if (!m_app) return false;

        const auto event_key = encode_key(sid, iid, eid);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_offered_events.insert(event_key).second) {
                m_app->offer_event(sid, iid, eid, {static_cast<vsomeip::eventgroup_t>(1)});
            }
        }
        auto payload = vsomeip::runtime::get()->create_payload();
        payload->set_data(data.data(), static_cast<std::uint32_t>(data.size()));

        m_app->notify(sid, iid, eid, payload);
        return true;
    }

    bool request_service(ServiceId sid, InstanceId iid,
                         MajorVersion major, MinorVersion minor) override {
        if (m_app) {
            m_app->request_service(sid, iid, major, minor);
            return true;
        }
        return false;
    }

    bool subscribe_event(ServiceId sid, InstanceId iid,
                         EventId eid, EventGroupId gid,
                         EventCallback callback) override {
        if (!m_app) return false;

        bool first_registration = false;
        bool service_available = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto key = encode_key(sid, iid, eid);
            first_registration = m_event_callbacks.find(key) == m_event_callbacks.end();
            m_event_callbacks[key] = std::move(callback);
            auto found = std::find_if(m_subscriptions.begin(), m_subscriptions.end(),
                [=](const Subscription& item) {
                    return item.sid == sid && item.iid == iid && item.eid == eid;
                });
            if (found == m_subscriptions.end()) m_subscriptions.push_back({sid, iid, eid, gid});
            else found->gid = gid;
            service_available = m_available_services.count(encode_key(sid, iid, 0)) != 0;
        }

        if (service_available) {
            m_app->request_event(sid, iid, eid, {gid});
            m_app->subscribe(
    sid, iid, gid,
    static_cast<vsomeip::major_version_t>(m_config.major_version));
        }

        // 注册通知处理函数
        if (first_registration) {
            m_app->register_subscription_status_handler(sid, iid, gid, eid,
                [](vsomeip::service_t service, vsomeip::instance_t instance,
                   vsomeip::eventgroup_t group, vsomeip::event_t event,
                   std::uint16_t status) {
                    ARA_LOG_INFO("VSOMEIP", "Subscription status service=" +
                        std::to_string(service) + " instance=" + std::to_string(instance) +
                        " group=" + std::to_string(group) + " event=" +
                        std::to_string(event) + " status=" + std::to_string(status));
                });
            m_app->register_message_handler(sid, iid, eid,
            [this, sid, iid, eid](const std::shared_ptr<vsomeip::message>& msg) {
                EventCallback callback;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    auto it = m_event_callbacks.find(encode_key(sid, iid, eid));
                    if (it != m_event_callbacks.end()) callback = it->second;
                }
                if (callback) {
                    auto payload = msg->get_payload();
                    if (payload) {
                        const auto* data = reinterpret_cast<const std::uint8_t*>(payload->get_data());
                        std::vector<std::uint8_t> vec(data, data + payload->get_length());
                        callback(vec);
                    }
                }
            });
        }

        return true;
    }

    ComRet call_method_sync(ServiceId sid, InstanceId iid,
                            MethodId mid,
                            const std::vector<std::uint8_t>& request,
                            std::vector<std::uint8_t>& response,
                            std::uint32_t timeout_ms) override {
        if (!m_app) return ComRet::NOT_AVAILABLE;

        ensure_response_handler(sid, iid, mid);
        auto req_msg = vsomeip::runtime::get()->create_request(true);
        req_msg->set_service(sid);
        req_msg->set_instance(iid);
        req_msg->set_method(mid);
        req_msg->set_message_type(vsomeip::message_type_e::MT_REQUEST);

        auto payload = vsomeip::runtime::get()->create_payload();
        payload->set_data(request.data(), static_cast<std::uint32_t>(request.size()));
        req_msg->set_payload(payload);

        auto pending = std::make_shared<PendingCall>();
        vsomeip::session_t session;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // vsomeip 在 send() 期间分配 client/session 标识；持锁避免响应早于登记
            m_app->send(req_msg);
            session = req_msg->get_session();
            ARA_LOG_DEBUG("VSOMEIP", "Sync request session=" + std::to_string(session));
            m_pending_calls[session] = pending;
        }

        std::unique_lock<std::mutex> lock(pending->mutex);
        if (!pending->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                  [&pending]() { return pending->done; })) {
            lock.unlock();
            std::lock_guard<std::mutex> map_lock(m_mutex);
            m_pending_calls.erase(session);
            return ComRet::TIMEOUT;
        }
        response = std::move(pending->payload);
        return pending->status;
    }

    bool call_method_async(ServiceId sid, InstanceId iid,
                           MethodId mid,
                           const std::vector<std::uint8_t>& request,
                           MethodReplyCallback callback,
                           std::uint32_t timeout_ms) override {
        if (!m_app) return false;

        ensure_response_handler(sid, iid, mid);
        auto req_msg = vsomeip::runtime::get()->create_request(true);
        req_msg->set_service(sid);
        req_msg->set_instance(iid);
        req_msg->set_method(mid);
        req_msg->set_message_type(vsomeip::message_type_e::MT_REQUEST);

        auto payload = vsomeip::runtime::get()->create_payload();
        payload->set_data(request.data(), static_cast<std::uint32_t>(request.size()));
        req_msg->set_payload(payload);

        vsomeip::session_t session;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_app->send(req_msg);
            session = req_msg->get_session();
            m_async_calls[session] = {
                std::move(callback),
                std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms)};
        }
        m_timeout_cv.notify_all();
        return true;
    }

    void register_availability_callback(AvailabilityCallback callback) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_availability_callbacks.push_back(std::move(callback));
    }

private:
    void on_registered() {
        ARA_LOG_INFO("VSOMEIP", "Application registered successfully");
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_registered = true;
        }
        m_state_cv.notify_all();
        std::vector<Subscription> events;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            events = m_registered_events;
        }
        for (const auto& event : events) {
            m_app->offer_event(event.sid, event.iid, event.eid, {event.gid});
        }
    }

    void ensure_response_handler(ServiceId sid, InstanceId iid, MethodId mid) {
        const auto key = encode_key(sid, iid, mid);
        bool first = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            first = m_response_handlers.insert(key).second;
        }
        if (first) {
            m_app->register_message_handler(sid, iid, mid,
                [this](const std::shared_ptr<vsomeip::message>& msg) { on_message(msg); });
        }
    }

    struct Subscription {
        ServiceId sid;
        InstanceId iid;
        EventId eid;
        EventGroupId gid;
    };

    void resubscribe(ServiceId sid, InstanceId iid) {
        std::vector<Subscription> subscriptions;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& item : m_subscriptions) {
                if (item.sid == sid && item.iid == iid) subscriptions.push_back(item);
            }
        }
        for (const auto& item : subscriptions) {
            ARA_LOG_DEBUG("VSOMEIP", "Subscribing event=" + std::to_string(item.eid) +
                          " group=" + std::to_string(item.gid));
            m_app->request_event(item.sid, item.iid, item.eid, {item.gid});
            m_app->subscribe(
    item.sid, item.iid, item.gid,
    static_cast<vsomeip::major_version_t>(m_config.major_version));
        }
    }

    void on_message(const std::shared_ptr<vsomeip::message>& msg) {
        if (!msg) return;

        if (msg->get_message_type() == vsomeip::message_type_e::MT_RESPONSE ||
            msg->get_message_type() == vsomeip::message_type_e::MT_ERROR) {
            complete_pending(msg);
            return;
        }

        if (msg->get_message_type() != vsomeip::message_type_e::MT_REQUEST &&
            msg->get_message_type() != vsomeip::message_type_e::MT_REQUEST_NO_RETURN) return;

        // 路由到对应 Method 处理函数
        auto key = encode_key(msg->get_service(), msg->get_instance(), msg->get_method());
        MethodHandler handler;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_method_handlers.find(key);
            if (it != m_method_handlers.end()) handler = it->second;
        }
        if (handler) {
            auto payload = msg->get_payload();
            std::vector<std::uint8_t> request_vec;
            if (payload) {
                const auto* d = reinterpret_cast<const std::uint8_t*>(payload->get_data());
                request_vec.assign(d, d + payload->get_length());
            }

            std::vector<std::uint8_t> response = handler(request_vec);

            // 发送响应
            auto resp_msg = vsomeip::runtime::get()->create_response(msg);

            auto resp_payload = vsomeip::runtime::get()->create_payload();
            resp_payload->set_data(response.data(), static_cast<std::uint32_t>(response.size()));
            resp_msg->set_payload(resp_payload);

            m_app->send(resp_msg);
        }
    }

    static std::uint64_t encode_key(ServiceId sid, InstanceId iid, MethodId mid) {
        return (static_cast<std::uint64_t>(sid) << 32) |
               (static_cast<std::uint64_t>(iid) << 16) |
               mid;
    }

    struct PendingCall {
        std::mutex mutex;
        std::condition_variable cv;
        bool done{false};
        ComRet status{ComRet::INTERNAL_ERR};
        std::vector<std::uint8_t> payload;
    };

    struct AsyncPendingCall {
        MethodReplyCallback callback;
        std::chrono::steady_clock::time_point deadline;
    };

    void timeout_loop() {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (m_running.load()) {
            if (m_async_calls.empty()) {
                m_timeout_cv.wait(lock, [this] {
                    return !m_running.load() || !m_async_calls.empty();
                });
                continue;
            }
            auto nearest = std::min_element(m_async_calls.begin(), m_async_calls.end(),
                [](const auto& lhs, const auto& rhs) {
                    return lhs.second.deadline < rhs.second.deadline;
                })->second.deadline;
            m_timeout_cv.wait_until(lock, nearest);
            if (!m_running.load()) break;

            const auto now = std::chrono::steady_clock::now();
            std::vector<MethodReplyCallback> expired;
            for (auto it = m_async_calls.begin(); it != m_async_calls.end();) {
                if (it->second.deadline <= now) {
                    expired.push_back(std::move(it->second.callback));
                    it = m_async_calls.erase(it);
                } else {
                    ++it;
                }
            }
            lock.unlock();
            for (auto& callback : expired) if (callback) callback(ComRet::TIMEOUT, {});
            lock.lock();
        }
    }

    static std::vector<std::uint8_t> payload_bytes(
        const std::shared_ptr<vsomeip::message>& msg) {
        const auto payload = msg->get_payload();
        if (!payload || payload->get_length() == 0) return {};
        const auto* data = reinterpret_cast<const std::uint8_t*>(payload->get_data());
        return {data, data + payload->get_length()};
    }

    void complete_pending(const std::shared_ptr<vsomeip::message>& msg) {
        const auto session = msg->get_session();
        ARA_LOG_DEBUG("VSOMEIP", "Response session=" + std::to_string(session));
        std::shared_ptr<PendingCall> sync;
        MethodReplyCallback async;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto sync_it = m_pending_calls.find(session);
            if (sync_it != m_pending_calls.end()) {
                sync = sync_it->second;
                m_pending_calls.erase(sync_it);
            } else {
                auto async_it = m_async_calls.find(session);
                if (async_it != m_async_calls.end()) {
                    async = std::move(async_it->second.callback);
                    m_async_calls.erase(async_it);
                    m_timeout_cv.notify_all();
                }
            }
        }
        const ComRet status = msg->get_message_type() == vsomeip::message_type_e::MT_RESPONSE
                                  ? ComRet::OK : ComRet::INTERNAL_ERR;
        auto bytes = payload_bytes(msg);
        if (sync) {
            std::lock_guard<std::mutex> lock(sync->mutex);
            sync->status = status;
            sync->payload = std::move(bytes);
            sync->done = true;
            sync->cv.notify_one();
        } else if (async) {
            async(status, bytes);
        }
    }

    void fail_all_pending(ComRet status) {
        std::map<vsomeip::session_t, std::shared_ptr<PendingCall>> sync;
        std::map<vsomeip::session_t, AsyncPendingCall> async;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            sync.swap(m_pending_calls);
            async.swap(m_async_calls);
        }
        for (auto& entry : sync) {
            auto& pending = entry.second;
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->status = status;
            pending->done = true;
            pending->cv.notify_one();
        }
        for (auto& entry : async) if (entry.second.callback) entry.second.callback(status, {});
    }

    ServiceConfig m_config;
    std::shared_ptr<vsomeip::application> m_app;
    std::vector<AvailabilityCallback> m_availability_callbacks;
    std::thread m_io_thread;
    std::thread m_timeout_thread;
    std::atomic<bool> m_running{false};
    std::mutex m_mutex;
    std::mutex m_state_mutex;
    std::condition_variable m_state_cv;
    bool m_registered{false};

    std::map<std::uint64_t, MethodHandler> m_method_handlers;
    std::map<std::uint64_t, EventCallback>  m_event_callbacks;
    std::set<std::uint64_t> m_offered_events;
    std::set<std::uint64_t> m_response_handlers;
    std::set<std::uint64_t> m_available_services;
    std::map<vsomeip::session_t, std::shared_ptr<PendingCall>> m_pending_calls;
    std::map<vsomeip::session_t, AsyncPendingCall> m_async_calls;
    std::condition_variable m_timeout_cv;
    std::vector<Subscription> m_subscriptions;
    std::vector<Subscription> m_registered_events;
};

#endif // ARA_COM_USE_VSOMEIP

// =========================================================================
//  后端工厂
// =========================================================================

std::shared_ptr<ICommunicationBackend> create_backend(BackendType type) {
    switch (type) {
#ifdef ARA_COM_USE_VSOMEIP
        case BackendType::VSOMEIP:
            return std::make_shared<VSomeIPBackend>();
#endif
        case BackendType::NULL_BACKEND:
        default:
            return std::make_shared<NullBackend>();
    }
}

} // namespace runtime
} // namespace com
} // namespace ara
