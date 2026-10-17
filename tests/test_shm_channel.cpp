/**
 * @file    test_shm_channel.cpp
 * @brief   共享内存通道单元测试。
 *
 * 同一进程里打开两个句柄模拟服务端/客户端，覆盖边界长度、回绕、满队列、
 * 超时、回调线程、客户端先启动、陈旧对象清理和析构清理。
 */

#include <gtest/gtest.h>

#include "ipc/shared_memory.h"
#include "ipc/shm_ring.h"

#if defined(ARA_COM_USE_POSIX_SHM)

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <semaphore.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

using ara::com::ipc::SHM_DEFAULT_CAPACITY;
using ara::com::ipc::SHM_MAX_MESSAGE_SIZE;
using ara::com::ipc::SharedMemoryChannel;

namespace {

class ShmChannelTest : public ::testing::Test {
protected:
    std::string name;

    void SetUp() override {
        static std::atomic<unsigned> counter{0};
        name = "test_" + std::to_string(::getpid()) + "_" +
               std::to_string(counter.fetch_add(1));
    }

    SharedMemoryChannel::Config config(std::size_t capacity = SHM_DEFAULT_CAPACITY,
                                       std::size_t max_message = SHM_MAX_MESSAGE_SIZE,
                                       std::uint32_t timeout_ms = 2000) const {
        SharedMemoryChannel::Config cfg;
        cfg.name = name;
        cfg.capacity = capacity;
        cfg.max_message = max_message;
        cfg.connect_timeout_ms = timeout_ms;
        return cfg;
    }

    static std::vector<std::uint8_t> pattern(std::size_t size, std::uint8_t seed) {
        std::vector<std::uint8_t> data(size);
        for (std::size_t i = 0; i < size; ++i) {
            data[i] = static_cast<std::uint8_t>(seed + i * 31U);
        }
        return data;
    }

    static std::string shm_name(const std::string& base, const char* direction) {
        return "/ara_com_" + base + "_" + direction;
    }
};

TEST_F(ShmChannelTest, BasicRoundTrip) {
    SharedMemoryChannel server(config());
    ASSERT_TRUE(server.init_server()) << server.last_error();

    SharedMemoryChannel client(config());
    ASSERT_TRUE(client.init_client()) << client.last_error();

    const std::vector<std::uint8_t> request{1, 2, 3, 4, 5};
    ASSERT_TRUE(client.send(request));

    std::vector<std::uint8_t> received;
    ASSERT_TRUE(server.receive(received));
    EXPECT_EQ(received, request);

    const std::vector<std::uint8_t> reply{9, 8, 7};
    ASSERT_TRUE(server.send(reply));
    ASSERT_TRUE(client.receive(received));
    EXPECT_EQ(received, reply);
}

TEST_F(ShmChannelTest, BoundaryMessageSizes) {
    SharedMemoryChannel server(config());
    ASSERT_TRUE(server.init_server()) << server.last_error();
    SharedMemoryChannel client(config());
    ASSERT_TRUE(client.init_client()) << client.last_error();

    const std::vector<std::size_t> sizes{0, 1, 7, 8, 9, 1024, 65536};
    for (std::size_t size : sizes) {
        const auto message = pattern(size, static_cast<std::uint8_t>(size));
        ASSERT_TRUE(client.send(message)) << "size=" << size;
        std::vector<std::uint8_t> received;
        ASSERT_TRUE(server.receive(received)) << "size=" << size;
        EXPECT_EQ(received, message) << "size=" << size;
    }
}

TEST_F(ShmChannelTest, WrapAroundKeepsRecordsIntact) {
    SharedMemoryChannel server(config(4096, 1024));
    ASSERT_TRUE(server.init_server()) << server.last_error();
    SharedMemoryChannel client(config(4096, 1024));
    ASSERT_TRUE(client.init_client()) << client.last_error();

    for (std::uint32_t sequence = 0; sequence < 500; ++sequence) {
        auto message = pattern(100, static_cast<std::uint8_t>(sequence));
        message[0] = static_cast<std::uint8_t>(sequence >> 8);
        message[1] = static_cast<std::uint8_t>(sequence & 0xFF);

        ASSERT_TRUE(client.send(message)) << "sequence=" << sequence;
        std::vector<std::uint8_t> received;
        ASSERT_TRUE(server.receive(received)) << "sequence=" << sequence;
        EXPECT_EQ(received, message) << "sequence=" << sequence;
    }
}

TEST_F(ShmChannelTest, FullQueueDoesNotOverwrite) {
    SharedMemoryChannel server(config(4096, 1024));
    ASSERT_TRUE(server.init_server()) << server.last_error();
    SharedMemoryChannel client(config(4096, 1024));
    ASSERT_TRUE(client.init_client()) << client.last_error();

    std::vector<std::vector<std::uint8_t>> sent;
    for (std::uint32_t sequence = 0;; ++sequence) {
        auto message = pattern(512, static_cast<std::uint8_t>(sequence));
        message[0] = static_cast<std::uint8_t>(sequence >> 8);
        message[1] = static_cast<std::uint8_t>(sequence & 0xFF);
        if (!client.send(message)) break;
        sent.push_back(std::move(message));
    }

    ASSERT_GT(sent.size(), 0U);
    EXPECT_FALSE(client.send(pattern(512, 0xEE)));

    std::vector<std::uint8_t> received;
    for (std::size_t i = 0; i < sent.size(); ++i) {
        ASSERT_TRUE(server.receive(received)) << "index=" << i;
        EXPECT_EQ(received, sent[i]) << "index=" << i;
    }
    EXPECT_FALSE(server.receive(received));

    EXPECT_TRUE(client.send(pattern(512, 0xAA)));
    ASSERT_TRUE(server.receive(received));
    EXPECT_EQ(received, pattern(512, 0xAA));
}

TEST_F(ShmChannelTest, WaitReceiveHonoursTimeout) {
    SharedMemoryChannel server(config());
    ASSERT_TRUE(server.init_server()) << server.last_error();
    SharedMemoryChannel client(config());
    ASSERT_TRUE(client.init_client()) << client.last_error();

    std::vector<std::uint8_t> received;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(server.wait_receive(received, 50));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    EXPECT_GE(elapsed.count(), 40);
    EXPECT_LT(elapsed.count(), 1000);
}

TEST_F(ShmChannelTest, ReceiverCallbackRunsUntilStopped) {
    SharedMemoryChannel server(config());
    ASSERT_TRUE(server.init_server()) << server.last_error();
    SharedMemoryChannel client(config());
    ASSERT_TRUE(client.init_client()) << client.last_error();

    std::atomic<int> received{0};

    server.set_receive_callback([&](const std::vector<std::uint8_t>& data) {
        EXPECT_EQ(data.size(), 4U);
        received.fetch_add(1, std::memory_order_release);
    });
    ASSERT_TRUE(server.start_receiver()) << server.last_error();

    for (std::uint32_t i = 0; i < 100; ++i) {
        ASSERT_TRUE(client.send(pattern(4, static_cast<std::uint8_t>(i))));
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (received.load(std::memory_order_acquire) < 100 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    server.stop_receiver();
    EXPECT_EQ(received.load(std::memory_order_acquire), 100);
}

TEST_F(ShmChannelTest, ClientCanStartBeforeServer) {
    std::atomic<bool> client_ready{false};
    std::string client_error;

    std::thread client_thread([&] {
        SharedMemoryChannel client(config(SHM_DEFAULT_CAPACITY, SHM_MAX_MESSAGE_SIZE, 3000));
        client_ready.store(client.init_client());
        if (!client_ready.load()) client_error = client.last_error();
        if (client_ready.load()) {
            EXPECT_TRUE(client.send({42, 43, 44}));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    SharedMemoryChannel server(config());
    ASSERT_TRUE(server.init_server()) << server.last_error();
    client_thread.join();

    ASSERT_TRUE(client_ready.load()) << client_error;

    std::vector<std::uint8_t> received;
    ASSERT_TRUE(server.wait_receive(received, 1000));
    EXPECT_EQ(received, std::vector<std::uint8_t>({42, 43, 44}));
}

TEST_F(ShmChannelTest, StaleRegionIsReclaimed) {
    const std::string stale = shm_name(name, "s2c");
    const int fd = ::shm_open(stale.c_str(), O_CREAT | O_RDWR, 0600);
    ASSERT_GE(fd, 0) << std::strerror(errno);
    const std::size_t size = ara::com::ipc::shm::region_size(4096);
    ASSERT_EQ(::ftruncate(fd, static_cast<off_t>(size)), 0);

    void* map = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_NE(map, MAP_FAILED);
    ara::com::ipc::shm::reset(static_cast<ara::com::ipc::shm::RingHeader*>(map),
                              4096, 1024,
                              std::numeric_limits<std::int32_t>::max());
    ::munmap(map, size);
    ::close(fd);

    SharedMemoryChannel server(config(4096, 1024));
    ASSERT_TRUE(server.init_server()) << server.last_error();

    SharedMemoryChannel client(config(4096, 1024));
    ASSERT_TRUE(client.init_client()) << client.last_error();
    ASSERT_TRUE(client.send({7, 7, 7}));
    std::vector<std::uint8_t> received;
    ASSERT_TRUE(server.receive(received));
    EXPECT_EQ(received, std::vector<std::uint8_t>({7, 7, 7}));
}

TEST_F(ShmChannelTest, DestructorUnlinksPosixObjects) {
    {
        SharedMemoryChannel server(config());
        ASSERT_TRUE(server.init_server()) << server.last_error();
    }

    const int fd = ::shm_open(shm_name(name, "s2c").c_str(), O_RDWR, 0600);
    EXPECT_LT(fd, 0);
    if (fd >= 0) ::close(fd);
    EXPECT_EQ(errno, ENOENT);

    sem_t* sem = ::sem_open(("/ara_com_" + name + "_s2c_data").c_str(), 0);
    EXPECT_EQ(sem, SEM_FAILED);
    if (sem != SEM_FAILED) ::sem_close(sem);
}

TEST_F(ShmChannelTest, RejectsOversizedMessage) {
    SharedMemoryChannel server(config());
    ASSERT_TRUE(server.init_server()) << server.last_error();
    SharedMemoryChannel client(config());
    ASSERT_TRUE(client.init_client()) << client.last_error();

    const std::vector<std::uint8_t> too_large(SHM_MAX_MESSAGE_SIZE + 1, 0x5A);
    EXPECT_FALSE(client.send(too_large));

    std::vector<std::uint8_t> received;
    EXPECT_FALSE(server.receive(received));
}

TEST_F(ShmChannelTest, RejectsConfigurationMismatch) {
    SharedMemoryChannel server(config(1024 * 1024, 64 * 1024));
    ASSERT_TRUE(server.init_server()) << server.last_error();

    SharedMemoryChannel client(config(4096, 1024));
    EXPECT_FALSE(client.init_client());
    EXPECT_NE(client.last_error().find("mismatch"), std::string::npos);
}

TEST_F(ShmChannelTest, RejectsInvalidNameAndDoubleInit) {
    SharedMemoryChannel::Config bad = config();
    bad.name = "../escape";
    SharedMemoryChannel invalid_name(bad);
    EXPECT_FALSE(invalid_name.init_server());

    SharedMemoryChannel server(config());
    ASSERT_TRUE(server.init_server()) << server.last_error();
    EXPECT_FALSE(server.init_server());
}

} // namespace

#endif  // ARA_COM_USE_POSIX_SHM
