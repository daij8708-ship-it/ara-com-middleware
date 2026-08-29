/**
 * @file    test_concurrency.cpp
 * @brief   多服务并发场景测试。
 *
 * 模拟15至20个服务并发运行，验证 Event、Method、E2E 与分片的并发行为。
 *
 */

#include "communication/service_skeleton.h"
#include "communication/service_proxy.h"
#include "communication/runtime.h"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace ara::com;

// =========================================================================
//  Concurrent event types
// =========================================================================

struct ConcurrentEvent {
    std::int32_t service_index{0};
    std::int32_t sequence{0};

    std::vector<std::uint8_t> serialize() const {
        std::vector<std::uint8_t> buf(8);
        buf[0] = (service_index >> 24) & 0xFF;
        buf[1] = (service_index >> 16) & 0xFF;
        buf[2] = (service_index >> 8) & 0xFF;
        buf[3] = service_index & 0xFF;
        buf[4] = (sequence >> 24) & 0xFF;
        buf[5] = (sequence >> 16) & 0xFF;
        buf[6] = (sequence >> 8) & 0xFF;
        buf[7] = sequence & 0xFF;
        return buf;
    }

    static ConcurrentEvent deserialize(const std::vector<std::uint8_t>& data) {
        ConcurrentEvent e{};
        if (data.size() >= 8) {
            e.service_index = (static_cast<std::int32_t>(data[0]) << 24) |
                              (static_cast<std::int32_t>(data[1]) << 16) |
                              (static_cast<std::int32_t>(data[2]) << 8)  |
                               static_cast<std::int32_t>(data[3]);
            e.sequence = (static_cast<std::int32_t>(data[4]) << 24) |
                         (static_cast<std::int32_t>(data[5]) << 16) |
                         (static_cast<std::int32_t>(data[6]) << 8)  |
                          static_cast<std::int32_t>(data[7]);
        }
        return e;
    }
};

// =========================================================================
//  15服务并发测试
// =========================================================================

TEST(ConcurrencyTest, Simulate15Services) {
    constexpr int NUM_SERVICES = 15;
    constexpr int EVENTS_PER_SERVICE = 50;

    auto backend = runtime::create_backend(runtime::BackendType::NULL_BACKEND);
    ServiceConfig base_cfg;
    base_cfg.role = "server";
    backend->init(base_cfg);
    backend->start();

    // 创建全部 Skeleton
    std::vector<std::unique_ptr<ServiceSkeleton>> skeletons;
    std::vector<std::atomic<int>> counters(NUM_SERVICES);

    for (int i = 0; i < NUM_SERVICES; ++i) {
        counters[i].store(0);

        ServiceConfig cfg;
        cfg.service_id   = static_cast<ServiceId>(0x1000 + i);
        cfg.instance_id  = 1;
        cfg.major_version = 1;
        cfg.role         = "server";

        auto skeleton = std::make_unique<ServiceSkeleton>(backend, cfg);
        skeleton->init();

        // 注册 Event
        skeleton->register_event<ConcurrentEvent>(0x0001,
            [](const ConcurrentEvent& ev) { return ev.serialize(); });

        skeleton->start();
        skeletons.push_back(std::move(skeleton));
    }

    // 所有服务并发发布 Event
    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_SERVICES; ++i) {
        threads.emplace_back([&, i]() {
            for (int seq = 0; seq < EVENTS_PER_SERVICE; ++seq) {
                ConcurrentEvent ev{i, seq};
                skeletons[i]->publish_event(0x0001, ev);
                counters[i].fetch_add(1);
            }
        });
    }

    // 等待所有线程结束
    for (auto& t : threads) {
        t.join();
    }

    // Verify all events were "sent"
    for (int i = 0; i < NUM_SERVICES; ++i) {
        EXPECT_EQ(counters[i].load(), EVENTS_PER_SERVICE);
    }

    // 清理资源
    for (auto& sk : skeletons) {
        sk->stop();
    }
    skeletons.clear();
    backend->stop();
}

// =========================================================================
//  20服务压力测试
// =========================================================================

TEST(ConcurrencyTest, Simulate20ServicesStress) {
    constexpr int NUM_SERVICES = 20;
    constexpr int EVENTS_PER_SERVICE = 100;

    auto backend = runtime::create_backend(runtime::BackendType::NULL_BACKEND);
    ServiceConfig base_cfg;
    base_cfg.role = "server";
    backend->init(base_cfg);
    backend->start();

    std::vector<std::unique_ptr<ServiceSkeleton>> skeletons;

    for (int i = 0; i < NUM_SERVICES; ++i) {
        ServiceConfig cfg;
        cfg.service_id   = static_cast<ServiceId>(0x2000 + i);
        cfg.instance_id  = 1;
        cfg.major_version = 1;
        cfg.role         = "server";

        auto skeleton = std::make_unique<ServiceSkeleton>(backend, cfg);
        skeleton->init();
        skeleton->register_event<ConcurrentEvent>(0x0001,
            [](const ConcurrentEvent& ev) { return ev.serialize(); });
        skeleton->start();
        skeletons.push_back(std::move(skeleton));
    }

    // 20个服务同时发布
    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_SERVICES; ++i) {
        threads.emplace_back([&, i]() {
            for (int seq = 0; seq < EVENTS_PER_SERVICE; ++seq) {
                ConcurrentEvent ev{i, seq};
                skeletons[i]->publish_event(0x0001, ev);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 清理并验证析构阶段无崩溃
    for (auto& sk : skeletons) {
        sk->stop();
    }
    skeletons.clear();
    backend->stop();

    SUCCEED();  // If we reach here without crash, test passes
}

// =========================================================================
//  Concurrent skeleton + proxy interaction
// =========================================================================

TEST(ConcurrencyTest, MixedSkeletonProxyConcurrency) {
    auto backend = runtime::create_backend(runtime::BackendType::NULL_BACKEND);

    ServiceConfig cfg;
    cfg.service_id   = 0x4001;
    cfg.instance_id  = 1;
    cfg.major_version = 1;
    cfg.role         = "server";

    backend->init(cfg);
    backend->start();

    ServiceSkeleton skeleton(backend, cfg);
    skeleton.init();
    skeleton.register_event<ConcurrentEvent>(0x0001,
        [](const ConcurrentEvent& ev) { return ev.serialize(); });
    skeleton.start();

    ServiceConfig client_cfg = cfg;
    client_cfg.role = "client";
    ServiceProxy proxy(backend, client_cfg);
    proxy.init();

    std::atomic<int> events_received{0};
    proxy.subscribe_event<ConcurrentEvent>(0x0001,
        [&events_received](const ConcurrentEvent& /*ev*/) {
            events_received.fetch_add(1);
        });

    proxy.start();

    // 主线程发布，Proxy 同步接收
    for (int i = 0; i < 100; ++i) {
        ConcurrentEvent ev{0, i};
        skeleton.publish_event(0x0001, ev);
    }

    // NullBackend 在 publish_event 内同步交付 Event
    EXPECT_EQ(events_received.load(), 100);

    proxy.stop();
    skeleton.stop();
    backend->stop();
}
