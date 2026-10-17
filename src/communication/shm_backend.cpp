/**
 * @file    shm_backend.cpp
 * @brief   同机共享内存后端实现。
 *
 * 控制面：服务端创建 <base>.ctl 通道，客户端串行发送 HELLO 注册，
 *         注册成功后服务端打开客户端预先创建的数据通道并回 ACK。
 * 数据面：每个客户端一条独立的双向通道，请求、响应、事件都走这条通道。
 */

#include "shm_backend.h"

#if defined(ARA_COM_USE_POSIX_SHM)

#include "common/logger.h"
#include "ipc/shared_memory.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <sys/file.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ara {
namespace com {
namespace runtime {

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t FRAME_MAGIC = 0x314D4853u;  // "SHM1"
constexpr std::uint16_t FRAME_VERSION = 1;
constexpr std::size_t FRAME_HEADER_SIZE = 24;
constexpr std::uint32_t CONNECT_TIMEOUT_MS = 500;
constexpr std::uint32_t ACK_TIMEOUT_MS = 1500;
constexpr std::uint32_t LOCK_TIMEOUT_MS = 1000;
constexpr std::size_t MAX_CLIENTS = 32;

enum class FrameType : std::uint16_t {
    HELLO = 1,
    ACK = 2,
    REQUEST = 3,
    RESPONSE = 4,
    EVENT = 5,
    SUBSCRIBE = 6,
    UNSUBSCRIBE = 7,
    BYE = 8
};

struct Frame {
    FrameType type{FrameType::HELLO};
    ServiceId sid{0};
    InstanceId iid{0};
    std::uint16_t id{0};
    SessionId session{0};
    std::uint16_t status{0};
    Bytes payload;
};

struct ShmLink {
    std::string name;
    std::shared_ptr<ipc::SharedMemoryChannel> channel;
    std::mutex events_mutex;
    std::set<EventId> events;
    std::atomic<bool> alive{true};
};

struct PendingSync {
    std::mutex mutex;
    std::condition_variable cv;
    bool done{false};
    ComRet status{ComRet::INTERNAL_ERR};
    Bytes payload;
};

struct PendingAsync {
    MethodReplyCallback callback;
    std::chrono::steady_clock::time_point deadline;
};

void put_u16(Bytes& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void put_u32(Bytes& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

std::uint16_t get_u16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0]) |
           static_cast<std::uint16_t>(data[1]) << 8;
}

std::uint32_t get_u32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) |
           static_cast<std::uint32_t>(data[1]) << 8 |
           static_cast<std::uint32_t>(data[2]) << 16 |
           static_cast<std::uint32_t>(data[3]) << 24;
}

Bytes encode_frame(FrameType type, ServiceId sid, InstanceId iid,
                   std::uint16_t id, SessionId session, std::uint16_t status,
                   const Bytes& payload) {
    Bytes out;
    out.reserve(FRAME_HEADER_SIZE + payload.size());
    put_u32(out, FRAME_MAGIC);
    put_u16(out, FRAME_VERSION);
    put_u16(out, static_cast<std::uint16_t>(type));
    put_u16(out, sid);
    put_u16(out, iid);
    put_u16(out, id);
    put_u16(out, session);
    put_u16(out, status);
    put_u16(out, 0);  // 保留字段，保证帧头 24 字节对齐
    put_u32(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

bool decode_frame(const Bytes& bytes, Frame& frame) {
    if (bytes.size() < FRAME_HEADER_SIZE) return false;
    const std::uint8_t* data = bytes.data();
    if (get_u32(data) != FRAME_MAGIC) return false;
    if (get_u16(data + 4) != FRAME_VERSION) return false;
    const auto type = static_cast<FrameType>(get_u16(data + 6));
    if (static_cast<std::uint16_t>(type) < 1 ||
        static_cast<std::uint16_t>(type) > 8) {
        return false;
    }
    const std::uint32_t payload_size = get_u32(data + 20);
    if (static_cast<std::size_t>(payload_size) != bytes.size() - FRAME_HEADER_SIZE) {
        return false;
    }
    frame.type = type;
    frame.sid = get_u16(data + 8);
    frame.iid = get_u16(data + 10);
    frame.id = get_u16(data + 12);
    frame.session = get_u16(data + 14);
    frame.status = get_u16(data + 16);
    frame.payload.assign(data + FRAME_HEADER_SIZE, data + bytes.size());
    return true;
}

bool valid_channel_name(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || ch == '_' || ch == '-' || ch == '.') {
            continue;
        }
        return false;
    }
    return true;
}

Bytes encode_hello(MajorVersion major, MinorVersion minor, const std::string& name) {
    Bytes payload;
    payload.push_back(major);
    put_u32(payload, minor);
    payload.insert(payload.end(), name.begin(), name.end());
    return payload;
}

bool decode_hello(const Bytes& payload, MajorVersion& major,
                  MinorVersion& minor, std::string& name) {
    if (payload.size() < 6) return false;
    major = payload[0];
    minor = get_u32(payload.data() + 1);
    name.assign(payload.begin() + 5, payload.end());
    return valid_channel_name(name);
}

ipc::SharedMemoryChannel::Config channel_config(const std::string& name) {
    ipc::SharedMemoryChannel::Config config;
    config.name = name;
    config.capacity = ipc::SHM_DEFAULT_CAPACITY;
    config.max_message = ipc::SHM_MAX_MESSAGE_SIZE;
    config.connect_timeout_ms = CONNECT_TIMEOUT_MS;
    return config;
}

std::uint64_t next_private_sequence() {
    static std::atomic<std::uint64_t> sequence{0};
    return ++sequence;
}

bool lock_file(int fd, std::uint32_t timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) return true;
        if (errno == EINTR) continue;
        if (errno != EWOULDBLOCK) return false;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

struct ShmBackend::Impl {
    ServiceConfig config;
    std::string base;
    std::string reg_lock_path;
    bool server_role{false};

    std::atomic<bool> running{false};
    std::atomic<bool> offered{false};
    MajorVersion offered_major{1};
    MinorVersion offered_minor{0};
    MajorVersion requested_major{1};
    MinorVersion requested_minor{0};

    mutable std::mutex state_mutex;
    std::vector<AvailabilityCallback> availability_callbacks;
    std::map<EventId, EventCallback> event_callbacks;
    std::map<EventId, EventGroupId> event_groups;

    // ---- 服务端 ----
    std::shared_ptr<ipc::SharedMemoryChannel> ctl;
    std::thread ctl_thread;
    std::thread prune_thread;
    int reg_lock_fd{-1};

    std::mutex links_mutex;
    std::map<std::string, std::shared_ptr<ShmLink>> links;

    std::mutex handlers_mutex;
    std::map<std::uint64_t, MethodHandler> method_handlers;
    std::set<EventId> registered_events;

    // ---- 客户端 ----
    std::shared_ptr<ipc::SharedMemoryChannel> data;
    std::shared_ptr<ipc::SharedMemoryChannel> client_ctl;
    std::thread connect_thread;
    std::thread timeout_thread;
    std::atomic<bool> wanted{false};
    std::atomic<bool> connected{false};
    std::atomic<bool> peer_bye{false};

    std::mutex channel_mutex;
    std::mutex pending_mutex;
    std::map<SessionId, std::shared_ptr<PendingSync>> pending_sync;
    std::map<SessionId, PendingAsync> pending_async;
    std::uint16_t session_counter{0};

    bool init(const ServiceConfig& cfg) {
        if (running.load()) return false;
        config = cfg;
        server_role = (cfg.role != "client");
        base = "svc_" + std::to_string(cfg.service_id) + "_" +
               std::to_string(cfg.instance_id);
        reg_lock_path = "/tmp/ara_com_" + base + "_reg.lock";
        offered_major = requested_major = cfg.major_version;
        offered_minor = requested_minor = cfg.minor_version;
        return true;
    }

    bool start() {
        if (running.exchange(true)) return true;

        if (server_role) {
            auto channel = std::make_shared<ipc::SharedMemoryChannel>(
                channel_config(base + ".ctl"));
            if (!channel->init_server()) {
                running.store(false);
                ARA_LOG_ERROR("SHM_BACKEND", "Failed to create control channel " + base);
                return false;
            }
            ctl = channel;

            reg_lock_fd = ::open(reg_lock_path.c_str(), O_CREAT | O_RDWR, 0600);
            if (reg_lock_fd < 0) {
                ARA_LOG_ERROR("SHM_BACKEND", "Failed to open registration lock " +
                              reg_lock_path + ": " + std::strerror(errno));
                ctl.reset();
                running.store(false);
                return false;
            }

            ctl_thread = std::thread([this]() { ctl_loop(); });
            prune_thread = std::thread([this]() { prune_loop(); });
            ARA_LOG_INFO("SHM_BACKEND", "Server started: " + base);
        } else {
            timeout_thread = std::thread([this]() { timeout_loop(); });
            connect_thread = std::thread([this]() { connect_loop(); });
            ARA_LOG_INFO("SHM_BACKEND", "Client started: " + base);
        }
        return true;
    }

    void stop() {
        running.store(false);

        if (server_role) {
            if (ctl_thread.joinable()) ctl_thread.join();
            if (prune_thread.joinable()) prune_thread.join();
            close_links(true);
            if (reg_lock_fd >= 0) {
                ::close(reg_lock_fd);
                reg_lock_fd = -1;
            }
            ctl.reset();
            ARA_LOG_INFO("SHM_BACKEND", "Server stopped: " + base);
        } else {
            wanted.store(false);
            if (connect_thread.joinable()) connect_thread.join();
            drop_connection();
            if (timeout_thread.joinable()) timeout_thread.join();
            fail_all_pending(ComRet::NOT_AVAILABLE);
            ARA_LOG_INFO("SHM_BACKEND", "Client stopped: " + base);
        }
    }

    bool offer_service(ServiceId sid, InstanceId iid,
                       MajorVersion major, MinorVersion minor) {
        if (!server_role) return false;
        if (sid != config.service_id || iid != config.instance_id) return false;
        {
            std::lock_guard<std::mutex> lock(handlers_mutex);
            offered_major = major;
            offered_minor = minor;
            offered.store(true);
        }
        ARA_LOG_INFO("SHM_BACKEND", "Offered service " + std::to_string(sid) + "." +
                     std::to_string(iid));
        return true;
    }

    void stop_offer_service(ServiceId sid, InstanceId iid) {
        if (!server_role) return;
        if (sid != config.service_id || iid != config.instance_id) return;
        offered.store(false);
        close_links(true);
        ARA_LOG_INFO("SHM_BACKEND", "Stopped offering service " +
                     std::to_string(sid) + "." + std::to_string(iid));
    }

    bool register_method_handler(ServiceId sid, InstanceId iid,
                                 MethodId mid, MethodHandler handler) {
        if (!server_role) return false;
        std::lock_guard<std::mutex> lock(handlers_mutex);
        method_handlers[encode_key(sid, iid, mid)] = std::move(handler);
        return true;
    }

    bool register_event(ServiceId, InstanceId, EventId eid, EventGroupId) {
        if (!server_role) return false;
        std::lock_guard<std::mutex> lock(handlers_mutex);
        registered_events.insert(eid);
        return true;
    }

    bool send_event(ServiceId sid, InstanceId iid,
                    EventId eid, const Bytes& payload) {
        if (!server_role || !offered.load()) return false;
        if (sid != config.service_id || iid != config.instance_id) return false;

        std::vector<std::shared_ptr<ShmLink>> targets;
        {
            std::lock_guard<std::mutex> lock(links_mutex);
            for (auto& entry : links) {
                if (entry.second->alive.load()) targets.push_back(entry.second);
            }
        }

        const Bytes frame = encode_frame(
            FrameType::EVENT, sid, iid, eid, 0, 0, payload);
        for (auto& link : targets) {
            bool subscribed = false;
            {
                std::lock_guard<std::mutex> lock(link->events_mutex);
                subscribed = link->events.count(eid) != 0;
            }
            if (!subscribed) continue;
            if (!link->channel->send(frame)) link->alive.store(false);
        }
        return true;
    }

    bool request_service(ServiceId sid, InstanceId iid,
                         MajorVersion major, MinorVersion minor) {
        if (server_role) return false;
        if (sid != config.service_id || iid != config.instance_id) return false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            requested_major = major;
            requested_minor = minor;
        }
        wanted.store(true);
        return true;
    }

    bool subscribe_event(ServiceId sid, InstanceId iid,
                         EventId eid, EventGroupId gid,
                         EventCallback callback) {
        if (server_role) return false;
        if (sid != config.service_id || iid != config.instance_id) return false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            event_callbacks[eid] = std::move(callback);
            event_groups[eid] = gid;
        }
        std::shared_ptr<ipc::SharedMemoryChannel> channel;
        {
            std::lock_guard<std::mutex> lock(channel_mutex);
            channel = data;
        }
        if (channel) {
            send_subscribe(channel, eid, gid);
        }
        return true;
    }

    ComRet call_method_sync(ServiceId sid, InstanceId iid,
                            MethodId mid, const Bytes& request,
                            Bytes& response, std::uint32_t timeout_ms) {
        if (server_role || !connected.load()) return ComRet::NOT_AVAILABLE;

        const SessionId session = next_session();
        if (session == 0) return ComRet::INTERNAL_ERR;

        auto pending = std::make_shared<PendingSync>();
        {
            std::lock_guard<std::mutex> lock(pending_mutex);
            pending_sync[session] = pending;
        }
        if (!send_client_frame(FrameType::REQUEST, mid, session, 0, request)) {
            std::lock_guard<std::mutex> lock(pending_mutex);
            pending_sync.erase(session);
            return ComRet::NOT_AVAILABLE;
        }

        std::unique_lock<std::mutex> lock(pending->mutex);
        // 用 system_clock 的 wait_until，libtsan 能正确看到等待期间释放 mutex；
        // steady_clock 的 wait_for 走 pthread_cond_clockwait，GCC 11 的 libtsan 不拦截。
        const auto deadline = std::chrono::system_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        if (!pending->cv.wait_until(lock, deadline,
                                    [&pending]() { return pending->done; })) {
            lock.unlock();
            std::lock_guard<std::mutex> guard(pending_mutex);
            pending_sync.erase(session);
            return ComRet::TIMEOUT;
        }
        response = std::move(pending->payload);
        return pending->status;
    }

    bool call_method_async(ServiceId sid, InstanceId iid,
                           MethodId mid, const Bytes& request,
                           MethodReplyCallback callback,
                           std::uint32_t timeout_ms) {
        if (server_role || !connected.load() || !callback) return false;

        const SessionId session = next_session();
        if (session == 0) return false;

        {
            std::lock_guard<std::mutex> lock(pending_mutex);
            pending_async[session] = {
                std::move(callback),
                std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms)};
        }
        if (!send_client_frame(FrameType::REQUEST, mid, session, 0, request)) {
            std::lock_guard<std::mutex> lock(pending_mutex);
            pending_async.erase(session);
            return false;
        }
        return true;
    }

    void register_availability_callback(AvailabilityCallback callback) {
        std::lock_guard<std::mutex> lock(state_mutex);
        availability_callbacks.push_back(std::move(callback));
    }

private:
    static std::uint64_t encode_key(ServiceId sid, InstanceId iid, MethodId mid) {
        return (static_cast<std::uint64_t>(sid) << 32) |
               (static_cast<std::uint64_t>(iid) << 16) | mid;
    }

    void notify_availability(ServiceAvailability availability) {
        std::vector<AvailabilityCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            callbacks = availability_callbacks;
        }
        for (auto& callback : callbacks) {
            if (!callback) continue;
            try {
                callback(config.service_id, config.instance_id, availability);
            } catch (...) {
                ARA_LOG_ERROR("SHM_BACKEND", "Availability callback threw");
            }
        }
    }

    void ctl_loop() {
        while (running.load()) {
            Bytes message;
            if (!ctl->wait_receive(message, 100)) continue;
            Frame frame;
            if (!decode_frame(message, frame)) continue;
            if (frame.type == FrameType::HELLO) handle_hello(frame);
        }
    }

    void handle_hello(const Frame& frame) {
        MajorVersion major = 0;
        MinorVersion minor = 0;
        std::string name;
        if (!decode_hello(frame.payload, major, minor, name)) {
            ARA_LOG_WARN("SHM_BACKEND", "Rejected malformed HELLO");
            return;
        }
        if (frame.sid != config.service_id || frame.iid != config.instance_id) return;
        {
            std::lock_guard<std::mutex> lock(handlers_mutex);
            if (!offered.load() || major != offered_major || minor != offered_minor) {
                ARA_LOG_WARN("SHM_BACKEND", "Rejected HELLO before offer or version mismatch");
                return;
            }
        }
        {
            std::lock_guard<std::mutex> lock(links_mutex);
            if (links.size() >= MAX_CLIENTS && links.count(name) == 0) {
                ARA_LOG_WARN("SHM_BACKEND", "Rejected client: limit reached");
                return;
            }
        }

        auto channel = std::make_shared<ipc::SharedMemoryChannel>(
            channel_config(name));
        if (!channel->init_client()) {
            ARA_LOG_WARN("SHM_BACKEND", "Failed to open client channel " + name);
            return;
        }

        auto link = std::make_shared<ShmLink>();
        link->name = name;
        link->channel = channel;
        channel->set_receive_callback(
            [this, weak = std::weak_ptr<ShmLink>(link)](const Bytes& bytes) {
                if (auto current = weak.lock()) handle_link_frame(current, bytes);
            });
        if (!channel->start_receiver()) {
            ARA_LOG_WARN("SHM_BACKEND", "Failed to start client receiver " + name);
            return;
        }

        std::shared_ptr<ShmLink> replaced;
        {
            // 与 stop_offer_service 的 offered 置位串行，避免停止发布后又插入新连接。
            std::lock_guard<std::mutex> handler_lock(handlers_mutex);
            if (!offered.load()) {
                channel->stop_receiver();
                return;
            }
            std::lock_guard<std::mutex> link_lock(links_mutex);
            auto it = links.find(name);
            if (it != links.end()) {
                replaced = it->second;
                it->second = link;
            } else {
                links[name] = link;
            }
        }
        if (replaced) {
            replaced->alive.store(false);
            replaced->channel->stop_receiver();
        }

        if (!channel->send(encode_frame(FrameType::ACK, config.service_id,
                                        config.instance_id, 0, 0, 0, {}))) {
            link->alive.store(false);
            ARA_LOG_WARN("SHM_BACKEND", "Failed to send ACK to " + name);
            return;
        }
        ARA_LOG_INFO("SHM_BACKEND", "Client registered: " + name);
    }

    void handle_link_frame(const std::shared_ptr<ShmLink>& link, const Bytes& bytes) {
        Frame frame;
        if (!decode_frame(bytes, frame)) return;
        if (frame.sid != config.service_id || frame.iid != config.instance_id) return;

        switch (frame.type) {
            case FrameType::REQUEST: {
                MethodHandler handler;
                {
                    std::lock_guard<std::mutex> lock(handlers_mutex);
                    auto it = method_handlers.find(
                        encode_key(frame.sid, frame.iid, frame.id));
                    if (it != method_handlers.end()) handler = it->second;
                }

                ComRet status = ComRet::OK;
                Bytes response;
                if (handler) {
                    try {
                        response = handler(frame.payload);
                    } catch (...) {
                        status = ComRet::INTERNAL_ERR;
                        response.clear();
                    }
                } else {
                    status = ComRet::NOT_AVAILABLE;
                }
                link->channel->send(encode_frame(
                    FrameType::RESPONSE, frame.sid, frame.iid, frame.id,
                    frame.session, static_cast<std::uint16_t>(status), response));
                break;
            }
            case FrameType::SUBSCRIBE: {
                std::lock_guard<std::mutex> lock(link->events_mutex);
                link->events.insert(frame.id);
                break;
            }
            case FrameType::UNSUBSCRIBE: {
                std::lock_guard<std::mutex> lock(link->events_mutex);
                link->events.erase(frame.id);
                break;
            }
            case FrameType::BYE:
                link->alive.store(false);
                break;
            default:
                break;
        }
    }

    void prune_loop() {
        while (running.load()) {
            for (int i = 0; i < 20 && running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            prune_links();
        }
    }

    void prune_links() {
        std::vector<std::shared_ptr<ShmLink>> dead;
        {
            std::lock_guard<std::mutex> lock(links_mutex);
            for (auto it = links.begin(); it != links.end();) {
                if (!it->second->alive.load() ||
                    !it->second->channel->is_connected()) {
                    dead.push_back(it->second);
                    it = links.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& link : dead) link->channel->stop_receiver();
    }

    void close_links(bool send_bye) {
        std::vector<std::shared_ptr<ShmLink>> old;
        {
            std::lock_guard<std::mutex> lock(links_mutex);
            for (auto& entry : links) old.push_back(entry.second);
            links.clear();
        }
        for (auto& link : old) {
            if (send_bye && link->alive.load()) {
                link->channel->send(encode_frame(
                    FrameType::BYE, config.service_id, config.instance_id,
                    0, 0, 0, {}));
            }
            link->alive.store(false);
            link->channel->stop_receiver();
        }
    }

    SessionId next_session() {
        std::lock_guard<std::mutex> lock(pending_mutex);
        for (unsigned i = 0; i < 65535; ++i) {
            const SessionId session = ++session_counter;
            if (session == 0) continue;
            if (pending_sync.count(session) == 0 &&
                pending_async.count(session) == 0) {
                return session;
            }
        }
        return 0;
    }

    bool send_client_frame(FrameType type, std::uint16_t id,
                           SessionId session, std::uint16_t status,
                           const Bytes& payload) {
        std::shared_ptr<ipc::SharedMemoryChannel> channel;
        {
            std::lock_guard<std::mutex> lock(channel_mutex);
            channel = data;
        }
        if (!channel) return false;
        return channel->send(encode_frame(type, config.service_id,
                                          config.instance_id, id, session,
                                          status, payload));
    }

    void send_subscribe(const std::shared_ptr<ipc::SharedMemoryChannel>& channel,
                        EventId eid, EventGroupId gid) {
        channel->send(encode_frame(FrameType::SUBSCRIBE, config.service_id,
                                   config.instance_id, eid,
                                   static_cast<SessionId>(gid), 0, {}));
    }

    void complete_pending(SessionId session, std::uint16_t status,
                          const Bytes& payload) {
        std::shared_ptr<PendingSync> sync;
        MethodReplyCallback async;
        {
            std::lock_guard<std::mutex> lock(pending_mutex);
            auto sync_it = pending_sync.find(session);
            if (sync_it != pending_sync.end()) {
                sync = sync_it->second;
                pending_sync.erase(sync_it);
            } else {
                auto async_it = pending_async.find(session);
                if (async_it != pending_async.end()) {
                    async = std::move(async_it->second.callback);
                    pending_async.erase(async_it);
                }
            }
        }

        ComRet result = ComRet::INTERNAL_ERR;
        if (status <= static_cast<std::uint16_t>(ComRet::NOT_CONNECTED)) {
            result = static_cast<ComRet>(status);
        }

        if (sync) {
            std::lock_guard<std::mutex> lock(sync->mutex);
            sync->status = result;
            sync->payload = payload;
            sync->done = true;
            sync->cv.notify_one();
        } else if (async) {
            try {
                async(result, payload);
            } catch (...) {
                ARA_LOG_ERROR("SHM_BACKEND", "Async reply callback threw");
            }
        }
    }

    void on_data_frame(const Bytes& bytes) {
        Frame frame;
        if (!decode_frame(bytes, frame)) return;
        if (frame.sid != config.service_id || frame.iid != config.instance_id) return;

        if (frame.type == FrameType::RESPONSE) {
            complete_pending(frame.session, frame.status, frame.payload);
            return;
        }
        if (frame.type == FrameType::EVENT) {
            EventCallback callback;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                auto it = event_callbacks.find(frame.id);
                if (it != event_callbacks.end()) callback = it->second;
            }
            if (callback) {
                try {
                    callback(frame.payload);
                } catch (...) {
                    ARA_LOG_ERROR("SHM_BACKEND", "Event callback threw");
                }
            }
            return;
        }
        if (frame.type == FrameType::BYE) {
            peer_bye.store(true);
        }
    }

    void timeout_loop() {
        // 10ms 轮询，避免 libstdc++ 的 clockwait 在 GCC 11 的 libtsan 下
        // 被当成"等待时仍持有 mutex"，污染后续的竞争判断。
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            std::vector<MethodReplyCallback> expired;
            {
                std::lock_guard<std::mutex> lock(pending_mutex);
                const auto now = std::chrono::steady_clock::now();
                for (auto it = pending_async.begin(); it != pending_async.end();) {
                    if (it->second.deadline <= now) {
                        expired.push_back(std::move(it->second.callback));
                        it = pending_async.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            for (auto& callback : expired) {
                if (!callback) continue;
                try {
                    callback(ComRet::TIMEOUT, {});
                } catch (...) {
                    ARA_LOG_ERROR("SHM_BACKEND", "Async timeout callback threw");
                }
            }
        }
    }

    void fail_all_pending(ComRet status) {
        std::map<SessionId, std::shared_ptr<PendingSync>> syncs;
        std::map<SessionId, PendingAsync> asyncs;
        {
            std::lock_guard<std::mutex> lock(pending_mutex);
            syncs.swap(pending_sync);
            asyncs.swap(pending_async);
        }
        for (auto& entry : syncs) {
            auto& pending = entry.second;
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->status = status;
            pending->done = true;
            pending->cv.notify_one();
        }
        for (auto& entry : asyncs) {
            if (!entry.second.callback) continue;
            try {
                entry.second.callback(status, {});
            } catch (...) {
                ARA_LOG_ERROR("SHM_BACKEND", "Async failure callback threw");
            }
        }
    }

    void connect_loop() {
        while (running.load()) {
            if (!wanted.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            if (!connected.load()) {
                if (try_connect()) {
                    connected.store(true);
                    notify_availability(ServiceAvailability::AVAILABLE);
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                continue;
            }

            const bool bye = peer_bye.exchange(false);
            bool ctl_alive = false;
            {
                std::lock_guard<std::mutex> lock(channel_mutex);
                ctl_alive = client_ctl && client_ctl->is_connected();
            }
            if (bye || !ctl_alive) {
                drop_connection();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }

    bool try_connect() {
        MajorVersion major = 1;
        MinorVersion minor = 0;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            major = requested_major;
            minor = requested_minor;
        }

        const std::string name = base + ".c" +
            std::to_string(static_cast<long long>(::getpid())) + "_" +
            std::to_string(next_private_sequence());
        auto channel = std::make_shared<ipc::SharedMemoryChannel>(
            channel_config(name));
        if (!channel->init_server()) return false;

        auto control = std::make_shared<ipc::SharedMemoryChannel>(
            channel_config(base + ".ctl"));
        if (!control->init_client()) return false;

        int fd = ::open(reg_lock_path.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd < 0) return false;
        bool sent = false;
        if (lock_file(fd, LOCK_TIMEOUT_MS)) {
            sent = control->send(encode_frame(
                FrameType::HELLO, config.service_id, config.instance_id, 0, 0, 0,
                encode_hello(major, minor, name)));
            ::flock(fd, LOCK_UN);
        }
        ::close(fd);
        if (!sent) return false;

        Bytes ack;
        if (!channel->wait_receive(ack, ACK_TIMEOUT_MS)) {
            channel->send(encode_frame(FrameType::BYE, config.service_id,
                                       config.instance_id, 0, 0, 0, {}));
            return false;
        }
        Frame frame;
        if (!decode_frame(ack, frame) || frame.type != FrameType::ACK) {
            channel->send(encode_frame(FrameType::BYE, config.service_id,
                                       config.instance_id, 0, 0, 0, {}));
            return false;
        }

        channel->set_receive_callback(
            [this](const Bytes& bytes) { on_data_frame(bytes); });
        if (!channel->start_receiver()) return false;

        {
            std::lock_guard<std::mutex> lock(channel_mutex);
            data = channel;
            client_ctl = control;
        }

        std::vector<std::pair<EventId, EventGroupId>> subscriptions;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            for (auto& entry : event_callbacks) {
                auto group = event_groups.find(entry.first);
                subscriptions.emplace_back(
                    entry.first,
                    group == event_groups.end() ? 1 : group->second);
            }
        }
        for (auto& entry : subscriptions) {
            send_subscribe(channel, entry.first, entry.second);
        }
        ARA_LOG_INFO("SHM_BACKEND", "Client connected via " + name);
        return true;
    }

    void drop_connection() {
        const bool was_connected = connected.exchange(false);
        std::shared_ptr<ipc::SharedMemoryChannel> old_data;
        std::shared_ptr<ipc::SharedMemoryChannel> old_ctl;
        {
            std::lock_guard<std::mutex> lock(channel_mutex);
            old_data.swap(data);
            old_ctl.swap(client_ctl);
        }
        if (old_data) {
            old_data->send(encode_frame(FrameType::BYE, config.service_id,
                                        config.instance_id, 0, 0, 0, {}));
            old_data->stop_receiver();
        }
        old_data.reset();
        old_ctl.reset();
        peer_bye.store(false);
        fail_all_pending(ComRet::NOT_AVAILABLE);
        if (was_connected) {
            notify_availability(ServiceAvailability::UNAVAILABLE);
            ARA_LOG_WARN("SHM_BACKEND", "Client disconnected: " + base);
        }
    }
};

ShmBackend::ShmBackend() : m_impl(std::make_unique<Impl>()) {}

ShmBackend::~ShmBackend() { stop(); }

bool ShmBackend::init(const ServiceConfig& config) {
    return m_impl->init(config);
}

bool ShmBackend::start() { return m_impl->start(); }

void ShmBackend::stop() { m_impl->stop(); }

bool ShmBackend::offer_service(ServiceId sid, InstanceId iid,
                               MajorVersion major, MinorVersion minor) {
    return m_impl->offer_service(sid, iid, major, minor);
}

void ShmBackend::stop_offer_service(ServiceId sid, InstanceId iid) {
    m_impl->stop_offer_service(sid, iid);
}

bool ShmBackend::register_method_handler(ServiceId sid, InstanceId iid,
                                         MethodId mid, MethodHandler handler) {
    return m_impl->register_method_handler(sid, iid, mid, std::move(handler));
}

bool ShmBackend::register_event(ServiceId sid, InstanceId iid,
                                EventId eid, EventGroupId gid) {
    return m_impl->register_event(sid, iid, eid, gid);
}

bool ShmBackend::send_event(ServiceId sid, InstanceId iid,
                            EventId eid, const std::vector<std::uint8_t>& data) {
    return m_impl->send_event(sid, iid, eid, data);
}

bool ShmBackend::request_service(ServiceId sid, InstanceId iid,
                                 MajorVersion major, MinorVersion minor) {
    return m_impl->request_service(sid, iid, major, minor);
}

bool ShmBackend::subscribe_event(ServiceId sid, InstanceId iid,
                                 EventId eid, EventGroupId gid,
                                 EventCallback callback) {
    return m_impl->subscribe_event(sid, iid, eid, gid, std::move(callback));
}

ComRet ShmBackend::call_method_sync(ServiceId sid, InstanceId iid,
                                    MethodId mid,
                                    const std::vector<std::uint8_t>& request,
                                    std::vector<std::uint8_t>& response,
                                    std::uint32_t timeout_ms) {
    return m_impl->call_method_sync(sid, iid, mid, request, response, timeout_ms);
}

bool ShmBackend::call_method_async(ServiceId sid, InstanceId iid,
                                   MethodId mid,
                                   const std::vector<std::uint8_t>& request,
                                   MethodReplyCallback callback,
                                   std::uint32_t timeout_ms) {
    return m_impl->call_method_async(sid, iid, mid, request,
                                     std::move(callback), timeout_ms);
}

void ShmBackend::register_availability_callback(AvailabilityCallback callback) {
    m_impl->register_availability_callback(std::move(callback));
}

} // namespace runtime
} // namespace com
} // namespace ara

#endif // ARA_COM_USE_POSIX_SHM
