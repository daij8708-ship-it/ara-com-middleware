/**
 * @file    test_proxy.cpp
 * @brief   ServiceProxy 单元测试。
 *
 * 验证 Proxy 初始化、Event 订阅、同步/异步 Method 调用和服务可用性处理。
 */

#include "communication/service_proxy.h"
#include "communication/service_skeleton.h"
#include "communication/runtime.h"
#include <gtest/gtest.h>

using namespace ara::com;

// =========================================================================
//  同时包含 Skeleton 和 Proxy 的测试夹具
// =========================================================================

class ServiceProxyTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 创建本地测试共享后端
        m_backend = runtime::create_backend(runtime::BackendType::NULL_BACKEND);

        ServiceConfig server_cfg;
        server_cfg.service_id   = 0x5678;
        server_cfg.instance_id  = 1;
        server_cfg.major_version = 1;
        server_cfg.role         = "server";

        ServiceConfig client_cfg;
        client_cfg.service_id   = 0x5678;
        client_cfg.instance_id  = 1;
        client_cfg.major_version = 1;
        client_cfg.role         = "client";

        m_backend->init(server_cfg);
        m_backend->start();

        m_skeleton = std::make_unique<ServiceSkeleton>(m_backend, server_cfg);
        m_proxy    = std::make_unique<ServiceProxy>(m_backend, client_cfg);
    }

    void TearDown() override {
        m_proxy.reset();
        m_skeleton.reset();
        m_backend->stop();
        m_backend.reset();
    }

    std::shared_ptr<runtime::ICommunicationBackend> m_backend;
    std::unique_ptr<ServiceSkeleton> m_skeleton;
    std::unique_ptr<ServiceProxy> m_proxy;
};

// 简单测试数据类型
struct PingRequest {
    std::string msg;

    std::vector<std::uint8_t> serialize() const {
        return std::vector<std::uint8_t>(msg.begin(), msg.end());
    }

    static PingRequest deserialize(const std::vector<std::uint8_t>& data) {
        PingRequest r;
        r.msg.assign(reinterpret_cast<const char*>(data.data()), data.size());
        return r;
    }
};

struct PongResponse {
    std::string echo;

    std::vector<std::uint8_t> serialize() const {
        return std::vector<std::uint8_t>(echo.begin(), echo.end());
    }

    static PongResponse deserialize(const std::vector<std::uint8_t>& data) {
        PongResponse r;
        r.echo.assign(reinterpret_cast<const char*>(data.data()), data.size());
        return r;
    }
};

struct SimpleEvent {
    std::int32_t id{0};

    std::vector<std::uint8_t> serialize() const {
        std::vector<std::uint8_t> buf(4);
        buf[0] = (id >> 24) & 0xFF; buf[1] = (id >> 16) & 0xFF;
        buf[2] = (id >> 8) & 0xFF;  buf[3] = id & 0xFF;
        return buf;
    }

    static SimpleEvent deserialize(const std::vector<std::uint8_t>& data) {
        SimpleEvent e;
        if (data.size() >= 4) {
            e.id = (static_cast<std::int32_t>(data[0]) << 24) |
                   (static_cast<std::int32_t>(data[1]) << 16) |
                   (static_cast<std::int32_t>(data[2]) << 8)  |
                    static_cast<std::int32_t>(data[3]);
        }
        return e;
    }
};

// =========================================================================
//  生命周期
// =========================================================================

TEST_F(ServiceProxyTest, InitAndStart) {
    EXPECT_TRUE(m_proxy->init());
    EXPECT_TRUE(m_proxy->start());
    EXPECT_EQ(m_proxy->service_id(), 0x5678);
}

TEST_F(ServiceProxyTest, InitWithoutStart) {
    EXPECT_TRUE(m_proxy->init());
    m_proxy->stop();  // 此时停止也不应崩溃
}

TEST_F(ServiceProxyTest, DoubleStop) {
    m_proxy->init();
    m_proxy->start();
    m_proxy->stop();
    m_proxy->stop();  // 重复停止应保持安全
}

// =========================================================================
//  Event 订阅
// =========================================================================

TEST_F(ServiceProxyTest, SubscribeEvent) {
    m_skeleton->init();
    m_skeleton->register_event<SimpleEvent>(0x0001);
    m_skeleton->set_e2e_enabled(false);
    m_skeleton->set_fragmentation_enabled(false);
    m_skeleton->start();
    m_proxy->init();
    m_proxy->set_e2e_enabled(false);
    m_proxy->set_fragmentation_enabled(false);
    m_proxy->start();

    std::atomic<bool> received{false};
    m_proxy->subscribe_event<SimpleEvent>(0x0001,
        [&received](const SimpleEvent& /*ev*/) {
            received.store(true);
        });

    ASSERT_TRUE(m_skeleton->publish_event(0x0001, SimpleEvent{42}));
    EXPECT_TRUE(received.load());
}

// =========================================================================
//  通过共享 NullBackend 同步调用 Method
// =========================================================================

TEST_F(ServiceProxyTest, SyncMethodCall) {
    // 配置服务端 Method
    m_skeleton->init();
    m_skeleton->register_method<PingRequest, PongResponse>(
        0x0202,
        [](const PingRequest& req) -> PongResponse {
            return PongResponse{"ECHO: " + req.msg};
        });
    m_skeleton->start();

    // 配置客户端
    m_proxy->init();
    m_proxy->start();

    PingRequest req{"hello"};
    auto rsp = m_proxy->call_method_sync<PingRequest, PongResponse>(0x0202, req);
    EXPECT_EQ(rsp.echo, "ECHO: hello");
}

// =========================================================================
//  服务可用性
// =========================================================================

TEST_F(ServiceProxyTest, AvailabilityCallback) {
    m_proxy->init();

    std::atomic<bool> notified{false};
    m_proxy->on_availability_change(
        [&notified](ServiceId /*sid*/, InstanceId /*iid*/, ServiceAvailability avail) {
            if (avail == ServiceAvailability::AVAILABLE) {
                notified.store(true);
            }
        });

    m_proxy->start();

    EXPECT_TRUE(notified.load());
    EXPECT_TRUE(m_proxy->is_available());
}

// =========================================================================
//  异步 Method 调用
// =========================================================================

TEST_F(ServiceProxyTest, AsyncMethodCall) {
    m_skeleton->init();
    m_skeleton->register_method<PingRequest, PongResponse>(
        0x0303,
        [](const PingRequest& req) -> PongResponse {
            return PongResponse{"ASYNC: " + req.msg};
        });
    m_skeleton->start();

    m_proxy->init();
    m_proxy->start();

    std::atomic<bool> callback_invoked{false};
    ComRet callback_status = ComRet::INTERNAL_ERR;

    PingRequest req{"world"};
    m_proxy->call_method_async<PingRequest, PongResponse>(
        0x0303, req,
        [&](ComRet status, const std::vector<std::uint8_t>& /*raw*/) {
            callback_invoked.store(true);
            callback_status = status;
        },
        2000);

    EXPECT_TRUE(callback_invoked.load());
    EXPECT_EQ(callback_status, ComRet::OK);

    int rejected_completions = 0;
    m_proxy->call_method_async<PingRequest, PongResponse>(
        0x7FFF, req, [&](ComRet status, const std::vector<std::uint8_t>&) {
            ++rejected_completions;
            EXPECT_EQ(status, ComRet::NOT_AVAILABLE);
        });
    EXPECT_EQ(rejected_completions, 1);
}

// =========================================================================
//  E2E 与分片开关
// =========================================================================

TEST_F(ServiceProxyTest, ToggleE2E) {
    m_proxy->init();
    m_proxy->set_e2e_enabled(false);
    m_proxy->start();
    EXPECT_TRUE(true);
}

TEST_F(ServiceProxyTest, ToggleFragmentation) {
    m_proxy->init();
    m_proxy->set_fragmentation_enabled(false);
    m_proxy->start();
    EXPECT_TRUE(true);
}
