#include "dds_backend.h"

#if defined(ARA_COM_USE_DDS)
#include "common/logger.h"

#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TopicDataType.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/rtps/transport/UDPv4TransportDescriptor.h>
#include <fastdds/rtps/common/SerializedPayload.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace ara::com::runtime {
namespace {
namespace dds = eprosima::fastdds::dds;
namespace rtps = eprosima::fastrtps::rtps;
using ReturnCode = eprosima::fastrtps::types::ReturnCode_t;
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Envelope {
    std::array<std::uint8_t, 16> client{};
    std::uint64_t request_id{0};
    std::uint16_t method{0};
    std::uint8_t status{0};
    Bytes payload;
};

class EnvelopeType final : public dds::TopicDataType {
public:
    EnvelopeType() {
        setName("ara_com_Envelope_v1");
        m_typeSize = DdsBackend::MAX_PAYLOAD_SIZE + 64;
        m_isGetKeyDefined = false;
        auto_fill_type_object(false);
        auto_fill_type_information(false);
    }

    bool serialize(void* data, rtps::SerializedPayload_t* payload) override {
        const auto& message = *static_cast<Envelope*>(data);
        if (message.payload.size() > DdsBackend::MAX_PAYLOAD_SIZE) return false;
        try {
            eprosima::fastcdr::FastBuffer buffer(
                reinterpret_cast<char*>(payload->data), payload->max_size);
            eprosima::fastcdr::Cdr cdr(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
                                      eprosima::fastcdr::Cdr::DDS_CDR);
            payload->encapsulation = cdr.endianness() == eprosima::fastcdr::Cdr::BIG_ENDIANNESS
                ? CDR_BE : CDR_LE;
            cdr.serialize_encapsulation();
            cdr.serializeArray(message.client.data(), message.client.size());
            cdr << message.request_id << message.method << message.status;
            cdr << static_cast<std::uint32_t>(message.payload.size());
            if (!message.payload.empty()) {
                cdr.serializeArray(message.payload.data(), message.payload.size());
            }
            payload->length = static_cast<std::uint32_t>(cdr.getSerializedDataLength());
            return true;
        } catch (const eprosima::fastcdr::exception::Exception&) {
            return false;
        }
    }

    bool deserialize(rtps::SerializedPayload_t* payload, void* data) override {
        if (payload->length > m_typeSize) return false;
        try {
            eprosima::fastcdr::FastBuffer buffer(
                reinterpret_cast<char*>(payload->data), payload->length);
            eprosima::fastcdr::Cdr cdr(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
                                      eprosima::fastcdr::Cdr::DDS_CDR);
            cdr.read_encapsulation();
            Envelope message;
            cdr.deserializeArray(message.client.data(), message.client.size());
            cdr >> message.request_id >> message.method >> message.status;
            std::uint32_t length = 0;
            cdr >> length;
            if (length > DdsBackend::MAX_PAYLOAD_SIZE ||
                message.status > static_cast<std::uint8_t>(ComRet::NOT_CONNECTED)) return false;
            const auto encoded_size = cdr.getSerializedDataLength() + length;
            // RTPS 在线上把 DATA 子消息补齐到四字节，进程内投递可能没有尾部填充。
            if (encoded_size > payload->length || payload->length - encoded_size > 3) return false;
            message.payload.resize(length);
            if (length != 0) cdr.deserializeArray(message.payload.data(), length);
            *static_cast<Envelope*>(data) = std::move(message);
            return true;
        } catch (const eprosima::fastcdr::exception::Exception&) {
            return false;
        }
    }

    std::function<std::uint32_t()> getSerializedSizeProvider(void* data) override {
        const auto size = static_cast<std::uint32_t>(static_cast<Envelope*>(data)->payload.size());
        return [size]() { return 36U + size; };
    }
    void* createData() override { return new Envelope; }
    void deleteData(void* data) override { delete static_cast<Envelope*>(data); }
    bool getKey(void*, rtps::InstanceHandle_t*, bool) override { return false; }
    bool is_bounded() const override { return true; }
};

void invoke_reply(const MethodReplyCallback& callback, ComRet status, const Bytes& data) {
    try {
        callback(status, data);
    } catch (const std::exception& e) {
        ARA_LOG_ERROR("DDS", std::string("应答回调异常：") + e.what());
    } catch (...) {
        ARA_LOG_ERROR("DDS", "应答回调发生未知异常");
    }
}

struct PendingCall {
    MethodId method;
    Clock::time_point deadline;
    MethodReplyCallback callback;
};

struct EventEndpoint {
    EventGroupId group;
    dds::DataWriter* writer{nullptr};
    dds::DataReader* reader{nullptr};
    EventCallback callback;
};

}

struct DdsBackend::Impl {
    explicit Impl(DdsBackendOptions value) : options(value), type(new EnvelopeType) {}

    DdsBackendOptions options;
    ServiceConfig config;
    bool initialized{false};
    bool offered{false};
    bool requested{false};
    std::uint64_t offer_generation{0};
    std::atomic<bool> running{false};
    bool available{false};
    std::mutex lifecycle_mutex;
    std::mutex entities_mutex;
    dds::DomainParticipant* participant{nullptr};
    dds::Publisher* publisher{nullptr};
    dds::Subscriber* subscriber{nullptr};
    dds::TypeSupport type;
    std::string prefix;
    std::map<std::string, dds::Topic*> topics;
    dds::DataWriter* request_writer{nullptr};
    dds::DataReader* request_reader{nullptr};
    dds::DataWriter* reply_writer{nullptr};
    dds::DataReader* reply_reader{nullptr};
    std::array<std::uint8_t, 16> client_id{};
    std::map<MethodId, MethodHandler> methods;
    std::map<EventId, EventEndpoint> events;
    std::vector<AvailabilityCallback> availability_callbacks;
    std::mutex pending_mutex;
    std::map<std::uint64_t, PendingCall> pending;
    std::uint64_t next_request{0};
    std::thread io_thread;
    std::thread timeout_thread;

    bool matches(ServiceId sid, InstanceId iid) const {
        return initialized && config.service_id == sid && config.instance_id == iid;
    }

    bool version_matches(MajorVersion major, MinorVersion minor) const {
        return config.major_version == major && config.minor_version == minor;
    }

    dds::Topic* topic(const std::string& suffix) {
        auto found = topics.find(suffix);
        if (found != topics.end()) return found->second;
        auto* value = participant->create_topic(prefix + suffix, type.get_type_name(),
                                                 dds::TOPIC_QOS_DEFAULT);
        if (value) topics.emplace(suffix, value);
        return value;
    }

    dds::DataWriter* writer(const std::string& suffix, bool event) {
        auto* endpoint_topic = topic(suffix);
        if (!endpoint_topic) return nullptr;
        dds::DataWriterQos qos;
        qos.reliability().kind = dds::RELIABLE_RELIABILITY_QOS;
        qos.reliability().max_blocking_time = {0, 100000000};
        // 小型有界历史需要及时确认，默认三秒心跳会让缓存先于 ACK 耗尽。
        qos.reliable_writer_qos().times.heartbeatPeriod = {0, 50000000};
        qos.durability().kind = dds::VOLATILE_DURABILITY_QOS;
        qos.history().kind = event ? dds::KEEP_LAST_HISTORY_QOS : dds::KEEP_ALL_HISTORY_QOS;
        qos.history().depth = 32;
        qos.resource_limits().max_samples = 128;
        qos.resource_limits().max_instances = 1;
        qos.resource_limits().max_samples_per_instance = 128;
        qos.resource_limits().allocated_samples = 16;
        qos.endpoint().history_memory_policy = rtps::PREALLOCATED_WITH_REALLOC_MEMORY_MODE;
        qos.data_sharing().off();
        return publisher->create_datawriter(endpoint_topic, qos);
    }

    dds::DataReader* reader(const std::string& suffix, bool event) {
        auto* endpoint_topic = topic(suffix);
        if (!endpoint_topic) return nullptr;
        dds::DataReaderQos qos;
        qos.reliability().kind = dds::RELIABLE_RELIABILITY_QOS;
        qos.durability().kind = dds::VOLATILE_DURABILITY_QOS;
        qos.history().kind = event ? dds::KEEP_LAST_HISTORY_QOS : dds::KEEP_ALL_HISTORY_QOS;
        qos.history().depth = 32;
        qos.resource_limits().max_samples = 128;
        qos.resource_limits().max_instances = 1;
        qos.resource_limits().max_samples_per_instance = 128;
        qos.resource_limits().allocated_samples = 16;
        qos.endpoint().history_memory_policy = rtps::PREALLOCATED_WITH_REALLOC_MEMORY_MODE;
        qos.data_sharing().off();
        return subscriber->create_datareader(endpoint_topic, qos);
    }

    std::string event_suffix(EventId eid, EventGroupId gid) const {
        return "_event_" + std::to_string(eid) + "_group_" + std::to_string(gid);
    }

    void destroy_entities() {
        if (participant) {
            participant->delete_contained_entities();
            dds::DomainParticipantFactory::get_instance()->delete_participant(participant);
        }
        participant = nullptr;
        publisher = nullptr;
        subscriber = nullptr;
        request_writer = nullptr;
        request_reader = nullptr;
        reply_writer = nullptr;
        reply_reader = nullptr;
        topics.clear();
        for (auto& entry : events) {
            entry.second.writer = nullptr;
            entry.second.reader = nullptr;
        }
    }

    bool start() {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex);
        std::lock_guard<std::mutex> lock(entities_mutex);
        if (!initialized) return false;
        if (running) return true;
        dds::DomainParticipantQos qos;
        qos.name("ara_com_" + config.role + "_" + std::to_string(config.service_id));
        // 强制 RTPS/UDP，避免同机优化把 DDS 验证变成另一条共享内存通路。
        qos.transport().use_builtin_transports = false;
        qos.transport().user_transports.push_back(
            std::make_shared<eprosima::fastdds::rtps::UDPv4TransportDescriptor>());
        qos.wire_protocol().builtin.discovery_config.leaseDuration = {3, 0};
        qos.wire_protocol().builtin.discovery_config.leaseDuration_announcementperiod = {1, 0};
        participant = dds::DomainParticipantFactory::get_instance()->create_participant(options.domain_id, qos);
        if (!participant) return false;
        if (type.register_type(participant) != ReturnCode::RETCODE_OK) {
            destroy_entities();
            return false;
        }
        publisher = participant->create_publisher(dds::PUBLISHER_QOS_DEFAULT);
        subscriber = participant->create_subscriber(dds::SUBSCRIBER_QOS_DEFAULT);
        if (!publisher || !subscriber) {
            destroy_entities();
            return false;
        }
        const auto& guid = participant->guid();
        std::copy_n(guid.guidPrefix.value, 12, client_id.begin());
        std::copy_n(guid.entityId.value, 4, client_id.begin() + 12);
        for (auto& entry : events) {
            auto& endpoint = entry.second;
            const auto suffix = event_suffix(entry.first, endpoint.group);
            if (config.role == "server") endpoint.writer = writer(suffix, true);
            else endpoint.reader = reader(suffix, true);
            if (!endpoint.writer && !endpoint.reader) {
                destroy_entities();
                return false;
            }
        }
        running = true;
        io_thread = std::thread([this]() { io_loop(); });
        if (config.role == "client") timeout_thread = std::thread([this]() { timeout_loop(); });
        return true;
    }

    void notify_availability(bool value) {
        std::vector<AvailabilityCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(entities_mutex);
            callbacks = availability_callbacks;
        }
        for (const auto& callback : callbacks) {
            try {
                callback(config.service_id, config.instance_id, value
                    ? ServiceAvailability::AVAILABLE : ServiceAvailability::UNAVAILABLE);
            } catch (const std::exception& e) {
                ARA_LOG_ERROR("DDS", std::string("可用性回调异常：") + e.what());
            } catch (...) {
                ARA_LOG_ERROR("DDS", "可用性回调发生未知异常");
            }
        }
    }

    void fail_pending() {
        std::map<std::uint64_t, PendingCall> cancelled;
        {
            std::lock_guard<std::mutex> lock(pending_mutex);
            cancelled.swap(pending);
        }
        for (const auto& entry : cancelled) invoke_reply(entry.second.callback, ComRet::NOT_AVAILABLE, {});
    }

    void stop() {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex);
        if (!running.exchange(false)) return;
        if (io_thread.joinable()) io_thread.join();
        if (timeout_thread.joinable()) timeout_thread.join();
        bool was_available;
        {
            std::lock_guard<std::mutex> lock(entities_mutex);
            was_available = available;
            available = false;
            offered = false;
            requested = false;
            ++offer_generation;
            destroy_entities();
        }
        fail_pending();
        if (was_available) notify_availability(false);
    }

    bool matched() const {
        if (!requested || !request_writer || !reply_reader) return false;
        dds::PublicationMatchedStatus requests;
        dds::SubscriptionMatchedStatus replies;
        if (request_writer->get_publication_matched_status(requests) != ReturnCode::RETCODE_OK ||
            reply_reader->get_subscription_matched_status(replies) != ReturnCode::RETCODE_OK) return false;
        return requests.current_count == 1 && replies.current_count == 1;
    }

    void complete(Envelope message) {
        if (message.client != client_id) return;
        PendingCall call;
        {
            std::lock_guard<std::mutex> lock(pending_mutex);
            const auto found = pending.find(message.request_id);
            if (found == pending.end() || found->second.method != message.method) return;
            call = std::move(found->second);
            pending.erase(found);
        }
        if (Clock::now() >= call.deadline) invoke_reply(call.callback, ComRet::TIMEOUT, {});
        else invoke_reply(call.callback, static_cast<ComRet>(message.status), message.payload);
    }

    void dispatch(Envelope message, std::uint64_t generation) {
        MethodHandler handler;
        {
            std::lock_guard<std::mutex> lock(entities_mutex);
            if (!offered || generation != offer_generation || !running) return;
            auto found = methods.find(message.method);
            if (found != methods.end()) handler = found->second;
        }
        try {
            if (handler) {
                message.payload = handler(message.payload);
                message.status = static_cast<std::uint8_t>(ComRet::OK);
                if (message.payload.size() > DdsBackend::MAX_PAYLOAD_SIZE) {
                    message.payload.clear();
                    message.status = static_cast<std::uint8_t>(ComRet::INTERNAL_ERR);
                }
            } else {
                message.payload.clear();
                message.status = static_cast<std::uint8_t>(ComRet::NOT_AVAILABLE);
            }
        } catch (...) {
            message.payload.clear();
            message.status = static_cast<std::uint8_t>(ComRet::INTERNAL_ERR);
        }
        std::lock_guard<std::mutex> lock(entities_mutex);
        if (running && offered && generation == offer_generation && reply_writer) {
            if (!reply_writer->write(&message)) ARA_LOG_WARN("DDS", "应答发送失败，请求将由客户端超时收尾");
        }
    }

    void io_loop() {
        while (running) {
            std::vector<Envelope> messages;
            std::vector<std::pair<EventCallback, Bytes>> deliveries;
            std::map<std::uint64_t, PendingCall> disconnected;
            bool changed = false;
            bool current = false;
            std::uint64_t generation = 0;
            {
                std::lock_guard<std::mutex> lock(entities_mutex);
                if (!running) break;
                if (config.role == "client") {
                    current = matched();
                    changed = current != available;
                    available = current;
                    if (changed && !current) {
                        std::lock_guard<std::mutex> calls(pending_mutex);
                        disconnected.swap(pending);
                    }
                }
                auto* input = config.role == "server" ? request_reader : reply_reader;
                generation = offer_generation;
                if (input) {
                    for (int i = 0; i < 32; ++i) {
                        Envelope message;
                        dds::SampleInfo info;
                        if (input->take_next_sample(&message, &info) != ReturnCode::RETCODE_OK) break;
                        if (info.valid_data) messages.push_back(std::move(message));
                    }
                }
                if (config.role == "client") {
                    for (auto& entry : events) {
                        auto& endpoint = entry.second;
                        if (!endpoint.reader) continue;
                        for (int i = 0; i < 32; ++i) {
                            Envelope message;
                            dds::SampleInfo info;
                            if (endpoint.reader->take_next_sample(&message, &info) != ReturnCode::RETCODE_OK) break;
                            if (info.valid_data && current && endpoint.callback) {
                                deliveries.emplace_back(endpoint.callback, std::move(message.payload));
                            }
                        }
                    }
                }
            }
            for (const auto& entry : disconnected) {
                invoke_reply(entry.second.callback, ComRet::NOT_AVAILABLE, {});
            }
            if (changed) notify_availability(current);
            for (auto& message : messages) {
                if (!running) break;
                if (config.role == "server") dispatch(std::move(message), generation);
                else complete(std::move(message));
            }
            for (auto& delivery : deliveries) {
                if (!running) break;
                try {
                    delivery.first(delivery.second);
                } catch (const std::exception& e) {
                    ARA_LOG_ERROR("DDS", std::string("事件回调异常：") + e.what());
                } catch (...) {
                    ARA_LOG_ERROR("DDS", "事件回调发生未知异常");
                }
            }
            std::this_thread::sleep_for(5ms);
        }
    }

    void timeout_loop() {
        while (running) {
            std::vector<MethodReplyCallback> expired;
            {
                std::lock_guard<std::mutex> lock(pending_mutex);
                const auto now = Clock::now();
                for (auto it = pending.begin(); it != pending.end();) {
                    if (it->second.deadline <= now) {
                        expired.push_back(std::move(it->second.callback));
                        it = pending.erase(it);
                    } else ++it;
                }
            }
            for (const auto& callback : expired) invoke_reply(callback, ComRet::TIMEOUT, {});
            std::this_thread::sleep_for(5ms);
        }
    }

    ComRet submit(ServiceId sid, InstanceId iid, MethodId mid, const Bytes& request,
                  MethodReplyCallback callback, std::uint32_t timeout_ms) {
        if (!callback || request.size() > DdsBackend::MAX_PAYLOAD_SIZE || timeout_ms == 0) {
            return ComRet::INTERNAL_ERR;
        }
        std::lock_guard<std::mutex> entities(entities_mutex);
        if (!running || config.role != "client" || !matches(sid, iid) || !matched()) {
            return ComRet::NOT_AVAILABLE;
        }
        std::lock_guard<std::mutex> calls(pending_mutex);
        if (pending.size() >= DdsBackend::MAX_PENDING_CALLS ||
            next_request == std::numeric_limits<std::uint64_t>::max()) return ComRet::INTERNAL_ERR;
        Envelope message;
        message.client = client_id;
        message.request_id = ++next_request;
        message.method = mid;
        message.payload = request;
        pending.emplace(message.request_id, PendingCall{mid, Clock::now() +
            std::chrono::milliseconds(timeout_ms), std::move(callback)});
        // write 返回前不允许超时线程取走记录，提交失败不会额外触发回调。
        if (!request_writer->write(&message)) {
            pending.erase(message.request_id);
            return ComRet::INTERNAL_ERR;
        }
        return ComRet::OK;
    }
};

DdsBackend::DdsBackend(DdsBackendOptions options) : impl_(std::make_unique<Impl>(options)) {}
DdsBackend::~DdsBackend() { impl_->stop(); }

bool DdsBackend::init(const ServiceConfig& config) {
    std::lock_guard<std::mutex> lock(impl_->entities_mutex);
    if (impl_->initialized || impl_->options.domain_id > 232 ||
        (config.role != "server" && config.role != "client")) return false;
    impl_->config = config;
    impl_->prefix = "ara_com_v1_s" + std::to_string(config.service_id) +
        "_i" + std::to_string(config.instance_id) + "_v" + std::to_string(config.major_version) +
        "_" + std::to_string(config.minor_version);
    impl_->initialized = true;
    return true;
}

bool DdsBackend::start() { return impl_->start(); }
void DdsBackend::stop() { impl_->stop(); }

bool DdsBackend::offer_service(ServiceId sid, InstanceId iid, MajorVersion major, MinorVersion minor) {
    std::lock_guard<std::mutex> lock(impl_->entities_mutex);
    if (!impl_->running || impl_->config.role != "server" || !impl_->matches(sid, iid) ||
        !impl_->version_matches(major, minor)) return false;
    if (impl_->offered) return true;
    impl_->reply_writer = impl_->writer("_reply", false);
    if (!impl_->reply_writer) return false;
    impl_->request_reader = impl_->reader("_request", false);
    if (!impl_->request_reader) {
        impl_->publisher->delete_datawriter(impl_->reply_writer);
        impl_->reply_writer = nullptr;
        return false;
    }
    ++impl_->offer_generation;
    impl_->offered = true;
    return true;
}

void DdsBackend::stop_offer_service(ServiceId sid, InstanceId iid) {
    std::lock_guard<std::mutex> lock(impl_->entities_mutex);
    if (!impl_->matches(sid, iid) || !impl_->offered) return;
    impl_->offered = false;
    ++impl_->offer_generation;
    impl_->subscriber->delete_datareader(impl_->request_reader);
    impl_->publisher->delete_datawriter(impl_->reply_writer);
    impl_->request_reader = nullptr;
    impl_->reply_writer = nullptr;
}

bool DdsBackend::register_method_handler(ServiceId sid, InstanceId iid, MethodId mid, MethodHandler handler) {
    std::lock_guard<std::mutex> lock(impl_->entities_mutex);
    if (!impl_->matches(sid, iid) || impl_->config.role != "server" || !handler) return false;
    impl_->methods[mid] = std::move(handler);
    return true;
}

bool DdsBackend::register_event(ServiceId sid, InstanceId iid, EventId eid, EventGroupId gid) {
    std::lock_guard<std::mutex> lock(impl_->entities_mutex);
    if (!impl_->matches(sid, iid) || impl_->config.role != "server") return false;
    auto found = impl_->events.find(eid);
    if (found != impl_->events.end()) return found->second.group == gid;
    EventEndpoint endpoint{gid, nullptr, nullptr, {}};
    if (impl_->running) {
        endpoint.writer = impl_->writer(impl_->event_suffix(eid, gid), true);
        if (!endpoint.writer) return false;
    }
    impl_->events.emplace(eid, std::move(endpoint));
    return true;
}

bool DdsBackend::send_event(ServiceId sid, InstanceId iid, EventId eid, const Bytes& data) {
    if (data.size() > MAX_PAYLOAD_SIZE) return false;
    std::lock_guard<std::mutex> lock(impl_->entities_mutex);
    if (!impl_->running || !impl_->offered || !impl_->matches(sid, iid)) return false;
    auto found = impl_->events.find(eid);
    if (found == impl_->events.end() || !found->second.writer) return false;
    Envelope message;
    message.method = eid;
    message.payload = data;
    return found->second.writer->write(&message);
}

bool DdsBackend::request_service(ServiceId sid, InstanceId iid, MajorVersion major, MinorVersion minor) {
    std::lock_guard<std::mutex> lock(impl_->entities_mutex);
    if (!impl_->running || impl_->config.role != "client" || !impl_->matches(sid, iid) ||
        !impl_->version_matches(major, minor)) return false;
    if (impl_->requested) return true;
    impl_->reply_reader = impl_->reader("_reply", false);
    if (!impl_->reply_reader) return false;
    impl_->request_writer = impl_->writer("_request", false);
    if (!impl_->request_writer) {
        impl_->subscriber->delete_datareader(impl_->reply_reader);
        impl_->reply_reader = nullptr;
        return false;
    }
    impl_->requested = true;
    return true;
}

bool DdsBackend::subscribe_event(ServiceId sid, InstanceId iid, EventId eid,
                                 EventGroupId gid, EventCallback callback) {
    std::lock_guard<std::mutex> lock(impl_->entities_mutex);
    if (!impl_->matches(sid, iid) || impl_->config.role != "client" || !callback) return false;
    auto found = impl_->events.find(eid);
    if (found != impl_->events.end()) {
        if (found->second.group != gid) return false;
        found->second.callback = std::move(callback);
        return true;
    }
    EventEndpoint endpoint{gid, nullptr, nullptr, std::move(callback)};
    if (impl_->running) {
        endpoint.reader = impl_->reader(impl_->event_suffix(eid, gid), true);
        if (!endpoint.reader) return false;
    }
    impl_->events.emplace(eid, std::move(endpoint));
    return true;
}

ComRet DdsBackend::call_method_sync(ServiceId sid, InstanceId iid, MethodId mid,
                                    const Bytes& request, Bytes& response, std::uint32_t timeout_ms) {
    struct Result {
        std::mutex mutex;
        std::condition_variable cv;
        bool done{false};
        ComRet status{ComRet::INTERNAL_ERR};
        Bytes data;
    };
    response.clear();
    auto result = std::make_shared<Result>();
    const auto status = impl_->submit(sid, iid, mid, request,
        [result](ComRet value, const Bytes& data) {
            {
                std::lock_guard<std::mutex> lock(result->mutex);
                result->status = value;
                result->data = data;
                result->done = true;
            }
            result->cv.notify_one();
        }, timeout_ms);
    if (status != ComRet::OK) return status;
    std::unique_lock<std::mutex> lock(result->mutex);
    result->cv.wait(lock, [&]() { return result->done; });
    response = std::move(result->data);
    return result->status;
}

bool DdsBackend::call_method_async(ServiceId sid, InstanceId iid, MethodId mid,
                                   const Bytes& request, MethodReplyCallback callback,
                                   std::uint32_t timeout_ms) {
    return impl_->submit(sid, iid, mid, request, std::move(callback), timeout_ms) == ComRet::OK;
}

void DdsBackend::register_availability_callback(AvailabilityCallback callback) {
    if (!callback) return;
    std::lock_guard<std::mutex> lock(impl_->entities_mutex);
    impl_->availability_callbacks.push_back(std::move(callback));
}

}
#endif
