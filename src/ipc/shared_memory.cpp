/**
 * @file    shared_memory.cpp
 * @brief   同机共享内存通道实现。
 *
 * 服务端创建两个方向的共享内存环和两个命名信号量；客户端打开同名对象。
 * 环的布局与收发算法在 shm_ring.h，本文件只负责 POSIX 对象生命周期、
 * 跨进程唤醒和线程安全封装。
 */

#include "shared_memory.h"
#include "common/logger.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

#if defined(ARA_COM_USE_POSIX_SHM)

#include "shm_ring.h"

#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

namespace ara {
namespace com {
namespace ipc {

namespace {

/// 通道名只允许出现在 POSIX 对象名中间，避免构造出非法路径。
bool valid_name(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || ch == '_' || ch == '-' || ch == '.') continue;
        return false;
    }
    return true;
}

bool pid_alive(std::int32_t pid) {
    if (pid <= 0) return false;
    if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno == EPERM;
}

std::string errno_text(const std::string& operation, int code) {
    return operation + ": " + std::strerror(code);
}

std::string shm_object(const std::string& base, const char* direction) {
    return "/ara_com_" + base + "_" + direction;
}

std::string sem_object(const std::string& base, const char* direction) {
    return "/ara_com_" + base + "_" + direction + "_data";
}

int wait_semaphore(sem_t* sem, std::uint32_t timeout_ms) {
    struct timespec deadline {};
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 30))
    if (::clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) return -1;
#else
    if (::clock_gettime(CLOCK_REALTIME, &deadline) != 0) return -1;
#endif
    deadline.tv_sec += static_cast<time_t>(timeout_ms / 1000U);
    deadline.tv_nsec += static_cast<long>(timeout_ms % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 30))
    return ::sem_clockwait(sem, CLOCK_MONOTONIC, &deadline);
#else
    return ::sem_timedwait(sem, &deadline);
#endif
}

} // namespace

struct SharedMemoryChannel::Impl {
    Config config;
    bool initialized{false};
    bool created{false};
    mutable std::atomic<bool> connected{false};
    std::atomic<bool> receiver_running{false};

    mutable std::mutex error_mutex;
    std::string last_error;

    std::mutex send_mutex;
    std::mutex recv_mutex;
    std::mutex callback_mutex;
    ReceiveCallback callback;
    std::thread receiver;

    struct Region {
        std::string shm_name;
        std::string sem_name;
        void* map{nullptr};
        std::size_t map_size{0};
        shm::RingHeader* header{nullptr};
        sem_t* sem{SEM_FAILED};
    };

    // 服务端：outgoing=s2c，incoming=c2s；客户端方向相反的使用方式相同。
    Region outgoing;
    Region incoming;

    ~Impl() { shutdown(); }

    void set_error(const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(error_mutex);
            last_error = message;
        }
        ARA_LOG_ERROR("SHM", message);
    }

    std::string error() const {
        std::lock_guard<std::mutex> lock(error_mutex);
        return last_error;
    }

    bool validate_config(std::string& error) const {
        if (!valid_name(config.name)) {
            error = "invalid channel name (allowed: A-Z a-z 0-9 _ - .; max 64 chars)";
            return false;
        }
        if (config.capacity < shm::MIN_CAPACITY ||
            config.capacity > shm::MAX_CAPACITY ||
            config.capacity % shm::RING_ALIGNMENT != 0) {
            error = "capacity must be a multiple of 8 in [4 KiB, 64 MiB]";
            return false;
        }
        if (config.max_message == 0 ||
            config.max_message > config.capacity - shm::RECORD_HEADER_SIZE) {
            error = "max_message must be in [1, capacity - 8]";
            return false;
        }
        return true;
    }

    enum class Existing { None, Live, Stale, Error };

    static Existing inspect_existing(const std::string& name, std::string& error) {
        const int fd = ::shm_open(name.c_str(), O_RDWR, 0600);
        if (fd < 0) {
            if (errno == ENOENT) return Existing::None;
            error = errno_text("shm_open(" + name + ")", errno);
            return Existing::Error;
        }

        struct stat info {};
        if (::fstat(fd, &info) != 0 ||
            static_cast<std::size_t>(info.st_size) < sizeof(shm::RingHeader)) {
            ::close(fd);
            return Existing::Stale;
        }

        void* map = ::mmap(nullptr, sizeof(shm::RingHeader), PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (map == MAP_FAILED) return Existing::Stale;

        auto* header = static_cast<shm::RingHeader*>(map);
        const std::int32_t creator = header->creator_pid;
        const bool alive = pid_alive(creator);
        ::munmap(map, sizeof(shm::RingHeader));

        if (!alive) return Existing::Stale;
        // magic 未发布但创建者还活着，说明服务端正在初始化；不能当成陈旧对象清掉。
        return Existing::Live;
    }

    static sem_t* create_semaphore(const std::string& name, std::string& error) {
        sem_t* sem = ::sem_open(name.c_str(), O_CREAT | O_EXCL, 0600, 0);
        if (sem == SEM_FAILED && errno == EEXIST) {
            // 共享内存已判定为陈旧/不存在，同名信号量也是残留对象。
            ::sem_unlink(name.c_str());
            sem = ::sem_open(name.c_str(), O_CREAT | O_EXCL, 0600, 0);
        }
        if (sem == SEM_FAILED) error = errno_text("sem_open(" + name + ")", errno);
        return sem;
    }

    static sem_t* open_semaphore(const std::string& name,
                                 std::chrono::steady_clock::time_point deadline,
                                 std::string& error) {
        for (;;) {
            sem_t* sem = ::sem_open(name.c_str(), 0);
            if (sem != SEM_FAILED) return sem;
            if (errno != ENOENT) {
                error = errno_text("sem_open(" + name + ")", errno);
                return SEM_FAILED;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                error = "timeout waiting for " + name;
                return SEM_FAILED;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    bool create_region(const std::string& name, Region& region, std::string& error) {
        const int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) {
            error = errno_text("shm_open(" + name + ")", errno);
            return false;
        }

        const std::size_t size = shm::region_size(config.capacity);
        if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
            error = errno_text("ftruncate(" + name + ")", errno);
            ::close(fd);
            ::shm_unlink(name.c_str());
            return false;
        }

        void* map = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (map == MAP_FAILED) {
            error = errno_text("mmap(" + name + ")", errno);
            ::shm_unlink(name.c_str());
            return false;
        }

        auto* header = static_cast<shm::RingHeader*>(map);
        shm::reset(header, config.capacity, config.max_message,
                   static_cast<std::int32_t>(::getpid()));
        region.shm_name = name;
        region.map = map;
        region.map_size = size;
        region.header = header;
        return true;
    }

    bool open_region(const std::string& name, Region& region,
                     std::chrono::steady_clock::time_point deadline,
                     std::string& error) {
        const std::size_t size = shm::region_size(config.capacity);
        for (;;) {
            const int fd = ::shm_open(name.c_str(), O_RDWR, 0600);
            if (fd >= 0) {
                struct stat info {};
                const bool big_enough =
                    (::fstat(fd, &info) == 0 &&
                     static_cast<std::size_t>(info.st_size) >= size);
                if (big_enough) {
                    void* map = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                    ::close(fd);
                    if (map != MAP_FAILED) {
                        auto* header = static_cast<shm::RingHeader*>(map);
                        if (shm::valid(header)) {
                            if (header->capacity != config.capacity ||
                                header->max_message != config.max_message) {
                                error = "channel '" + config.name +
                                        "' configuration mismatch (capacity/max_message)";
                                ::munmap(map, size);
                                return false;
                            }
                            region.shm_name = name;
                            region.map = map;
                            region.map_size = size;
                            region.header = header;
                            return true;
                        }
                        ::munmap(map, size);
                    }
                } else {
                    ::close(fd);
                }
            } else if (errno != ENOENT) {
                error = errno_text("shm_open(" + name + ")", errno);
                return false;
            }

            if (std::chrono::steady_clock::now() >= deadline) {
                error = "timeout waiting for " + name;
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    static void close_region(Region& region) {
        if (region.sem != SEM_FAILED) {
            ::sem_close(region.sem);
            region.sem = SEM_FAILED;
        }
        if (region.map != nullptr) {
            ::munmap(region.map, region.map_size);
        }
        region.map = nullptr;
        region.map_size = 0;
        region.header = nullptr;
    }

    void shutdown() {
        stop_receiver();

        if (created) {
            if (!outgoing.shm_name.empty()) ::shm_unlink(outgoing.shm_name.c_str());
            if (!incoming.shm_name.empty()) ::shm_unlink(incoming.shm_name.c_str());
            if (!outgoing.sem_name.empty()) ::sem_unlink(outgoing.sem_name.c_str());
            if (!incoming.sem_name.empty()) ::sem_unlink(incoming.sem_name.c_str());
            created = false;
        }

        close_region(outgoing);
        close_region(incoming);
        initialized = false;
        connected.store(false);
    }

    bool init_server() {
        if (initialized) {
            set_error("channel already initialized");
            return false;
        }

        std::string error;
        if (!validate_config(error)) {
            set_error(error);
            return false;
        }

        const std::string s2c = shm_object(config.name, "s2c");
        const std::string c2s = shm_object(config.name, "c2s");
        const std::string s2c_sem = sem_object(config.name, "s2c");
        const std::string c2s_sem = sem_object(config.name, "c2s");

        const Existing s2c_existing = inspect_existing(s2c, error);
        if (s2c_existing == Existing::Error) {
            set_error(error);
            return false;
        }
        const Existing c2s_existing = inspect_existing(c2s, error);
        if (c2s_existing == Existing::Error) {
            set_error(error);
            return false;
        }
        if (s2c_existing == Existing::Live || c2s_existing == Existing::Live) {
            set_error("shared memory channel '" + config.name + "' is already in use");
            return false;
        }
        if (s2c_existing == Existing::Stale || c2s_existing == Existing::Stale) {
            ::shm_unlink(s2c.c_str());
            ::shm_unlink(c2s.c_str());
            ::sem_unlink(s2c_sem.c_str());
            ::sem_unlink(c2s_sem.c_str());
        }

        // O_EXCL 创建共享内存同时充当"同名通道只有一个服务端"的互斥：
        // 第二个服务端会在 create_region 失败，而不会碰到第一个服务端的信号量。
        if (!create_region(s2c, outgoing, error)) {
            set_error(error);
            return false;
        }
        if (!create_region(c2s, incoming, error)) {
            close_region(outgoing);
            ::shm_unlink(s2c.c_str());
            set_error(error);
            return false;
        }

        // 共享内存先可见、信号量后创建；客户端打开共享内存后会按超时重试信号量。
        sem_t* s2c_data = create_semaphore(s2c_sem, error);
        if (s2c_data == SEM_FAILED) {
            close_region(outgoing);
            close_region(incoming);
            ::shm_unlink(s2c.c_str());
            ::shm_unlink(c2s.c_str());
            set_error(error);
            return false;
        }
        sem_t* c2s_data = create_semaphore(c2s_sem, error);
        if (c2s_data == SEM_FAILED) {
            ::sem_close(s2c_data);
            ::sem_unlink(s2c_sem.c_str());
            close_region(outgoing);
            close_region(incoming);
            ::shm_unlink(s2c.c_str());
            ::shm_unlink(c2s.c_str());
            set_error(error);
            return false;
        }

        outgoing.sem = s2c_data;
        outgoing.sem_name = s2c_sem;
        incoming.sem = c2s_data;
        incoming.sem_name = c2s_sem;

        created = true;
        initialized = true;
        connected.store(true);
        ARA_LOG_INFO("SHM", "server channel ready: " + config.name);
        return true;
    }

    bool init_client() {
        if (initialized) {
            set_error("channel already initialized");
            return false;
        }

        std::string error;
        if (!validate_config(error)) {
            set_error(error);
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(config.connect_timeout_ms);
        const std::string s2c = shm_object(config.name, "s2c");
        const std::string c2s = shm_object(config.name, "c2s");
        const std::string s2c_sem = sem_object(config.name, "s2c");
        const std::string c2s_sem = sem_object(config.name, "c2s");

        // 客户端发送走 c2s，接收走 s2c，与服务端方向相反。
        if (!open_region(c2s, outgoing, deadline, error)) {
            set_error(error);
            return false;
        }
        if (!open_region(s2c, incoming, deadline, error)) {
            close_region(outgoing);
            set_error(error);
            return false;
        }

        sem_t* c2s_data = open_semaphore(c2s_sem, deadline, error);
        if (c2s_data == SEM_FAILED) {
            close_region(outgoing);
            close_region(incoming);
            set_error(error);
            return false;
        }
        sem_t* s2c_data = open_semaphore(s2c_sem, deadline, error);
        if (s2c_data == SEM_FAILED) {
            ::sem_close(c2s_data);
            close_region(outgoing);
            close_region(incoming);
            set_error(error);
            return false;
        }

        outgoing.sem = c2s_data;
        outgoing.sem_name = c2s_sem;
        incoming.sem = s2c_data;
        incoming.sem_name = s2c_sem;

        initialized = true;
        connected.store(true);
        ARA_LOG_INFO("SHM", "client channel connected: " + config.name);
        return true;
    }

    bool peer_available() const {
        if (!initialized || !connected.load()) return false;
        if (shm::load_state(outgoing.header) != shm::RING_STATE_OK ||
            shm::load_state(incoming.header) != shm::RING_STATE_OK) {
            connected.store(false);
            return false;
        }
        if (!created) {
            const std::int32_t creator = incoming.header->creator_pid;
            if (!pid_alive(creator)) {
                connected.store(false);
                return false;
            }
        }
        return true;
    }

    bool send(const std::vector<std::uint8_t>& data) {
        if (!initialized) {
            set_error("channel not initialized");
            return false;
        }
        if (!connected.load()) return false;
        if (data.size() > config.max_message) {
            set_error("message exceeds max_message: " + std::to_string(data.size()));
            return false;
        }

        std::lock_guard<std::mutex> lock(send_mutex);
        if (!shm::write(outgoing.header, data.data(), data.size())) {
            return false;  // 队列满不计错误；dropped 已在环内累加。
        }
        if (::sem_post(outgoing.sem) != 0) {
            // 数据已经入环；接收侧的 100 ms 超时轮询仍能取到，只记录唤醒失败。
            set_error(errno_text("sem_post", errno));
        }
        return true;
    }

    bool receive(std::vector<std::uint8_t>& data) {
        if (!initialized) {
            set_error("channel not initialized");
            return false;
        }

        std::lock_guard<std::mutex> lock(recv_mutex);
        if (!shm::read(incoming.header, data)) {
            if (shm::load_state(incoming.header) != shm::RING_STATE_OK) {
                connected.store(false);
                set_error("shared memory ring marked broken");
            }
            return false;
        }
        return true;
    }

    bool wait_receive(std::vector<std::uint8_t>& data, std::uint32_t timeout_ms) {
        if (!initialized) {
            set_error("channel not initialized");
            return false;
        }
        if (receive(data)) return true;
        if (timeout_ms == 0) return false;

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            const auto wait_ms = static_cast<std::uint32_t>(remaining.count() > 0
                                                                ? remaining.count()
                                                                : 1);
            const int rc = wait_semaphore(incoming.sem, wait_ms);
            if (rc == 0) {
                if (receive(data)) return true;
                continue;
            }
            if (errno == EINTR) continue;
            if (errno == ETIMEDOUT) break;
            set_error(errno_text("sem_wait", errno));
            break;
        }

        if (!peer_available()) return false;
        return receive(data);
    }

    bool is_connected() const { return peer_available(); }

    bool start_receiver() {
        if (!initialized) {
            set_error("channel not initialized");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            if (!callback) {
                set_error("receive callback is not set");
                return false;
            }
        }
        if (receiver_running.exchange(true)) return true;

        receiver = std::thread([this]() {
            while (receiver_running.load(std::memory_order_acquire)) {
                std::vector<std::uint8_t> data;
                if (wait_receive(data, 100)) {
                    ReceiveCallback current;
                    {
                        std::lock_guard<std::mutex> lock(callback_mutex);
                        current = callback;
                    }
                    if (current) current(data);
                } else if (!created && !connected.load()) {
                    break;  // 客户端发现服务端消失后自行退出
                }
            }
        });
        return true;
    }

    void stop_receiver() {
        receiver_running.store(false, std::memory_order_release);
        if (receiver.joinable()) receiver.join();
    }

    Stats stats() const {
        Stats result;
        if (!initialized) return result;

        auto fill = [](const Region& region, DirectionStats& out) {
            if (region.header == nullptr) return;
            out.published = shm::load_acquire(&region.header->published);
            out.consumed = shm::load_acquire(&region.header->consumed);
            out.dropped = shm::load_acquire(&region.header->dropped);
            const std::uint64_t head = shm::load_acquire(&region.header->head);
            const std::uint64_t tail = shm::load_acquire(&region.header->tail);
            if (tail >= head && tail - head <= region.header->capacity) {
                out.queued = tail - head;
            }
        };

        fill(outgoing, result.outgoing);
        fill(incoming, result.incoming);
        return result;
    }
};

} // namespace ipc
} // namespace com
} // namespace ara

#else  // !ARA_COM_USE_POSIX_SHM

namespace ara {
namespace com {
namespace ipc {

struct SharedMemoryChannel::Impl {
    Config config;
    std::string last_error;
    ReceiveCallback callback;

    bool init_server() {
        last_error = "POSIX shared memory is not enabled at build time";
        ARA_LOG_ERROR("SHM", last_error);
        return false;
    }
    bool init_client() { return init_server(); }
    bool send(const std::vector<std::uint8_t>&) { return false; }
    bool receive(std::vector<std::uint8_t>&) { return false; }
    bool wait_receive(std::vector<std::uint8_t>&, std::uint32_t) { return false; }
    bool is_connected() const { return false; }
    bool start_receiver() { return false; }
    void stop_receiver() {}
    Stats stats() const { return {}; }
    std::string error() const { return last_error; }
};

} // namespace ipc
} // namespace com
} // namespace ara

#endif  // ARA_COM_USE_POSIX_SHM

// ---- 公共转发：两种构建下保持同一套接口 ----

namespace ara {
namespace com {
namespace ipc {

SharedMemoryChannel::SharedMemoryChannel(Config config)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->config = std::move(config);
}

SharedMemoryChannel::SharedMemoryChannel(const std::string& name)
    : SharedMemoryChannel(Config{name}) {}

SharedMemoryChannel::~SharedMemoryChannel() = default;

bool SharedMemoryChannel::init_server() { return m_impl->init_server(); }

bool SharedMemoryChannel::init_client() { return m_impl->init_client(); }

bool SharedMemoryChannel::send(const std::vector<std::uint8_t>& data) {
    return m_impl->send(data);
}

bool SharedMemoryChannel::receive(std::vector<std::uint8_t>& data) {
    return m_impl->receive(data);
}

bool SharedMemoryChannel::wait_receive(std::vector<std::uint8_t>& data,
                                       std::uint32_t timeout_ms) {
    return m_impl->wait_receive(data, timeout_ms);
}

bool SharedMemoryChannel::is_connected() const { return m_impl->is_connected(); }

void SharedMemoryChannel::set_receive_callback(ReceiveCallback callback) {
#if defined(ARA_COM_USE_POSIX_SHM)
    std::lock_guard<std::mutex> lock(m_impl->callback_mutex);
#endif
    m_impl->callback = std::move(callback);
}

bool SharedMemoryChannel::start_receiver() { return m_impl->start_receiver(); }

void SharedMemoryChannel::stop_receiver() { m_impl->stop_receiver(); }

SharedMemoryChannel::Stats SharedMemoryChannel::stats() const {
    return m_impl->stats();
}

std::string SharedMemoryChannel::last_error() const { return m_impl->error(); }

const std::string& SharedMemoryChannel::name() const { return m_impl->config.name; }

} // namespace ipc
} // namespace com
} // namespace ara
