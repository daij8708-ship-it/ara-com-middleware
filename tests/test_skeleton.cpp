/**
 * @file    test_skeleton.cpp
 * @brief   ServiceSkeleton 单元测试。
 *
 * 验证服务初始化、启停生命周期、Event 发布和 Method 注册。
 */

#include "communication/service_skeleton.h"
#include "communication/runtime.h"
#include "common/json_parser.h"
#include <gtest/gtest.h>

using namespace ara::com;

// =========================================================================
//  测试夹具：使用 NullBackend 创建 Skeleton
// =========================================================================

class ServiceSkeletonTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_backend = runtime::create_backend(runtime::BackendType::NULL_BACKEND);

        m_config.service_id   = 0x1234;
        m_config.instance_id  = 1;
        m_config.major_version = 1;
        m_config.minor_version = 0;
        m_config.ip_address   = "127.0.0.1";
        m_config.port         = 30500;
        m_config.protocol     = "UDP";
        m_config.role         = "server";

        m_backend->init(m_config);
        m_backend->start();

        m_skeleton = std::make_unique<ServiceSkeleton>(m_backend, m_config);
    }

    void TearDown() override {
        m_skeleton.reset();
        m_backend->stop();
        m_backend.reset();
    }

    std::shared_ptr<runtime::ICommunicationBackend> m_backend;
    ServiceConfig m_config;
    std::unique_ptr<ServiceSkeleton> m_skeleton;
};

// =========================================================================
//  生命周期
// =========================================================================

TEST_F(ServiceSkeletonTest, InitAndStart) {
    EXPECT_TRUE(m_skeleton->init());
    EXPECT_TRUE(m_skeleton->start());
    EXPECT_EQ(m_skeleton->service_id(), 0x1234);
    EXPECT_EQ(m_skeleton->instance_id(), 1);
}

TEST_F(ServiceSkeletonTest, InitWithoutStart) {
    EXPECT_TRUE(m_skeleton->init());
    // 未启动时调用 stop() 应保持安全
    m_skeleton->stop();  // Should not crash
}

TEST_F(ServiceSkeletonTest, StartWithoutInit) {
    EXPECT_FALSE(m_skeleton->start());  // Should fail
}

TEST_F(ServiceSkeletonTest, DoubleInit) {
    EXPECT_TRUE(m_skeleton->init());
    EXPECT_TRUE(m_skeleton->init());  // Should be idempotent
}

// =========================================================================
//  Event 注册与发布
// =========================================================================

// 简单测试数据类型
struct TestEvent {
    std::int32_t value{0};

    std::vector<std::uint8_t> serialize() const {
        std::vector<std::uint8_t> buf(4);
        buf[0] = (value >> 24) & 0xFF;
        buf[1] = (value >> 16) & 0xFF;
        buf[2] = (value >> 8) & 0xFF;
        buf[3] = value & 0xFF;
        return buf;
    }
};

TEST_F(ServiceSkeletonTest, RegisterAndPublishEvent) {
    m_skeleton->init();

    // 使用自定义序列化函数注册 Event
    m_skeleton->register_event<TestEvent>(0x0001,
        [](const TestEvent& ev) { return ev.serialize(); });

    EXPECT_TRUE(m_skeleton->start());

    TestEvent ev{42};
    // 发布过程不应抛出异常
    EXPECT_TRUE(m_skeleton->publish_event(0x0001, ev));
}

TEST_F(ServiceSkeletonTest, PublishUnregisteredEvent) {
    m_skeleton->init();
    m_skeleton->start();

    TestEvent ev{100};
    // 未注册序列化函数的 Event 应发布失败
    EXPECT_FALSE(m_skeleton->publish_event(0x9999, ev));
}

TEST_F(ServiceSkeletonTest, PublishBeforeStart) {
    m_skeleton->init();
    // 服务未启动时应发布失败
    TestEvent ev{10};
    EXPECT_FALSE(m_skeleton->publish_event(0x0001, ev));
}

// =========================================================================
//  Method 注册
// =========================================================================

struct TestRequest {
    std::int32_t a{0};
    std::int32_t b{0};

    std::vector<std::uint8_t> serialize() const {
        std::vector<std::uint8_t> buf(8);
        buf[0] = (a >> 24) & 0xFF; buf[1] = (a >> 16) & 0xFF;
        buf[2] = (a >> 8) & 0xFF;  buf[3] = a & 0xFF;
        buf[4] = (b >> 24) & 0xFF; buf[5] = (b >> 16) & 0xFF;
        buf[6] = (b >> 8) & 0xFF;  buf[7] = b & 0xFF;
        return buf;
    }

    static TestRequest deserialize(const std::vector<std::uint8_t>& data) {
        TestRequest r{};
        if (data.size() >= 8) {
            r.a = (static_cast<std::int32_t>(data[0]) << 24) |
                  (static_cast<std::int32_t>(data[1]) << 16) |
                  (static_cast<std::int32_t>(data[2]) << 8)  |
                   static_cast<std::int32_t>(data[3]);
            r.b = (static_cast<std::int32_t>(data[4]) << 24) |
                  (static_cast<std::int32_t>(data[5]) << 16) |
                  (static_cast<std::int32_t>(data[6]) << 8)  |
                   static_cast<std::int32_t>(data[7]);
        }
        return r;
    }
};

struct TestResponse {
    std::int32_t sum{0};

    std::vector<std::uint8_t> serialize() const {
        std::vector<std::uint8_t> buf(4);
        buf[0] = (sum >> 24) & 0xFF; buf[1] = (sum >> 16) & 0xFF;
        buf[2] = (sum >> 8) & 0xFF;  buf[3] = sum & 0xFF;
        return buf;
    }

    static TestResponse deserialize(const std::vector<std::uint8_t>& data) {
        TestResponse r{};
        if (data.size() >= 4) {
            r.sum = (static_cast<std::int32_t>(data[0]) << 24) |
                    (static_cast<std::int32_t>(data[1]) << 16) |
                    (static_cast<std::int32_t>(data[2]) << 8)  |
                     static_cast<std::int32_t>(data[3]);
        }
        return r;
    }
};

TEST_F(ServiceSkeletonTest, RegisterMethod) {
    m_skeleton->init();
    m_skeleton->start();

    // 注册加法 Method
    m_skeleton->register_method<TestRequest, TestResponse>(
        0x0101,
        [](const TestRequest& req) -> TestResponse {
            return TestResponse{req.a + req.b};
        });

    // No exception = success
    SUCCEED();
}

// =========================================================================
//  E2E 与分片开关
// =========================================================================

TEST_F(ServiceSkeletonTest, DisableE2E) {
    m_skeleton->init();
    m_skeleton->set_e2e_enabled(false);
    m_skeleton->start();

    TestEvent ev{1};
    m_skeleton->register_event<TestEvent>(0x0001,
        [](const TestEvent& e) { return e.serialize(); });
    EXPECT_TRUE(m_skeleton->publish_event(0x0001, ev));
}

TEST_F(ServiceSkeletonTest, DisableFragmentation) {
    m_skeleton->init();
    m_skeleton->set_fragmentation_enabled(false);
    m_skeleton->start();

    TestEvent ev{2};
    m_skeleton->register_event<TestEvent>(0x0002,
        [](const TestEvent& e) { return e.serialize(); });
    EXPECT_TRUE(m_skeleton->publish_event(0x0002, ev));
}

TEST_F(ServiceSkeletonTest, SetCustomMTU) {
    m_skeleton->init();
    m_skeleton->set_mtu(512);
    m_skeleton->start();

    TestEvent ev{3};
    m_skeleton->register_event<TestEvent>(0x0003,
        [](const TestEvent& e) { return e.serialize(); });
    EXPECT_TRUE(m_skeleton->publish_event(0x0003, ev));
}
