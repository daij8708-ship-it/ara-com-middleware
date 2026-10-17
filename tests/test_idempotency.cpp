/**
 * @file    test_idempotency.cpp
 * @brief   同步 RPC 至多一次语义单元测试（NullBackend）。
 *
 * 覆盖：请求 key 编解码、去重窗口、同 key 只执行一次、不同 key 各执行一次、
 * 窗口淘汰后重放会重新执行（已知边界）、默认关闭不加前缀、key 会被正确剥离。
 *
 * 说明：NullBackend 的同步调用不产生超时，因此"超时后同 key 重试"的重试循环
 * 无法在这里自然触发；真实重试路径需要 vsomeip（见 docs/run-all-tests.md）。
 */

#include "communication/service_proxy.h"
#include "communication/service_skeleton.h"
#include "communication/runtime.h"
#include "safety/idempotency.h"
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <vector>

using namespace ara::com;

namespace {

constexpr ServiceId kSid = 0x5151;
constexpr InstanceId kIid = 1;
constexpr MethodId kMethod = 0x0808;

struct AddRequest {
    std::int32_t a{0};
    std::int32_t b{0};

    std::vector<std::uint8_t> serialize() const {
        return {static_cast<std::uint8_t>((a >> 24) & 0xFF),
                static_cast<std::uint8_t>((a >> 16) & 0xFF),
                static_cast<std::uint8_t>((a >> 8) & 0xFF),
                static_cast<std::uint8_t>(a & 0xFF),
                static_cast<std::uint8_t>((b >> 24) & 0xFF),
                static_cast<std::uint8_t>((b >> 16) & 0xFF),
                static_cast<std::uint8_t>((b >> 8) & 0xFF),
                static_cast<std::uint8_t>(b & 0xFF)};
    }

    static AddRequest deserialize(const std::vector<std::uint8_t>& data) {
        AddRequest r;
        if (data.size() >= 8) {
            r.a = static_cast<std::int32_t>((static_cast<std::uint32_t>(data[0]) << 24) |
                                            (static_cast<std::uint32_t>(data[1]) << 16) |
                                            (static_cast<std::uint32_t>(data[2]) << 8)  |
                                             static_cast<std::uint32_t>(data[3]));
            r.b = static_cast<std::int32_t>((static_cast<std::uint32_t>(data[4]) << 24) |
                                            (static_cast<std::uint32_t>(data[5]) << 16) |
                                            (static_cast<std::uint32_t>(data[6]) << 8)  |
                                             static_cast<std::uint32_t>(data[7]));
        }
        return r;
    }
};

struct AddResponse {
    std::int32_t sum{0};

    std::vector<std::uint8_t> serialize() const {
        return {static_cast<std::uint8_t>((sum >> 24) & 0xFF),
                static_cast<std::uint8_t>((sum >> 16) & 0xFF),
                static_cast<std::uint8_t>((sum >> 8) & 0xFF),
                static_cast<std::uint8_t>(sum & 0xFF)};
    }

    static AddResponse deserialize(const std::vector<std::uint8_t>& data) {
        AddResponse r;
        if (data.size() >= 4) {
            r.sum = static_cast<std::int32_t>((static_cast<std::uint32_t>(data[0]) << 24) |
                                              (static_cast<std::uint32_t>(data[1]) << 16) |
                                              (static_cast<std::uint32_t>(data[2]) << 8)  |
                                               static_cast<std::uint32_t>(data[3]));
        }
        return r;
    }
};

// 按"key 前缀 + 业务负载"拼出一条请求
std::vector<std::uint8_t> framed_request(std::uint64_t key, const AddRequest& body) {
    std::vector<std::uint8_t> request = safety::encode_request_key(key);
    const auto payload = body.serialize();
    request.insert(request.end(), payload.begin(), payload.end());
    return request;
}

} // namespace

class IdempotencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_backend = runtime::create_backend(runtime::BackendType::NULL_BACKEND);
        ServiceConfig server_cfg;
        server_cfg.service_id    = kSid;
        server_cfg.instance_id   = kIid;
        server_cfg.major_version = 1;
        server_cfg.role          = "server";
        m_client_cfg = server_cfg;
        m_client_cfg.role = "client";

        m_backend->init(server_cfg);
        m_backend->start();

        m_skeleton = std::make_unique<ServiceSkeleton>(m_backend, server_cfg);
        m_proxy    = std::make_unique<ServiceProxy>(m_backend, m_client_cfg);
    }

    void TearDown() override {
        m_proxy.reset();
        m_skeleton.reset();
        m_backend->stop();
        m_backend.reset();
    }

    std::shared_ptr<runtime::ICommunicationBackend> m_backend;
    ServiceConfig m_client_cfg;
    std::unique_ptr<ServiceSkeleton> m_skeleton;
    std::unique_ptr<ServiceProxy> m_proxy;
};

// =========================================================================
//  纯函数：key 编解码与窗口
// =========================================================================

TEST(IdempotencyKeyTest, EncodingRoundTrip) {
    const std::uint64_t key = 0x0123456789ABCDEFULL;
    const auto bytes = safety::encode_request_key(key);
    ASSERT_EQ(bytes.size(), safety::REQUEST_KEY_SIZE);
    EXPECT_EQ(safety::decode_request_key(bytes), key);
}

TEST(IdempotencyWindowTest, RecordsAndLooksUp) {
    safety::IdempotencyWindow window(4);
    std::vector<std::uint8_t> out;
    EXPECT_FALSE(window.lookup(7, out));

    window.record(7, {0x01, 0x02});
    ASSERT_TRUE(window.lookup(7, out));
    EXPECT_EQ(out, (std::vector<std::uint8_t>{0x01, 0x02}));
    EXPECT_FALSE(window.lookup(8, out));
}

TEST(IdempotencyWindowTest, EvictsOldestBeyondCapacity) {
    safety::IdempotencyWindow window(2);
    window.record(1, {0x11});
    window.record(2, {0x22});
    window.record(3, {0x33});  // 挤掉 1

    std::vector<std::uint8_t> out;
    EXPECT_FALSE(window.lookup(1, out)) << "最旧的应已被淘汰";
    EXPECT_TRUE(window.lookup(2, out));
    EXPECT_TRUE(window.lookup(3, out));
    EXPECT_EQ(window.size(), 2u);
}

// =========================================================================
//  服务端去重
// =========================================================================

TEST_F(IdempotencyTest, DuplicateKeyIsServedFromCache) {
    m_skeleton->init();
    m_skeleton->set_rpc_idempotency_enabled(true);
    m_skeleton->set_idempotency_window_size(8);

    std::atomic<int> calls{0};
    m_skeleton->register_method<AddRequest, AddResponse>(kMethod,
        [&](const AddRequest& r) { ++calls; return AddResponse{r.a + r.b}; });
    ASSERT_TRUE(m_skeleton->start());

    const auto request = framed_request(0xABCDEF0123456789ULL, AddRequest{2, 3});

    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    ASSERT_EQ(m_backend->call_method_sync(kSid, kIid, kMethod, request, first, 2000), ComRet::OK);
    ASSERT_EQ(m_backend->call_method_sync(kSid, kIid, kMethod, request, second, 2000), ComRet::OK);

    EXPECT_EQ(calls.load(), 1) << "同一个 key 的第二次请求应回缓存，不重执行";
    EXPECT_EQ(first, second);
}

TEST_F(IdempotencyTest, DistinctKeysBothExecute) {
    m_skeleton->init();
    m_skeleton->set_rpc_idempotency_enabled(true);
    m_skeleton->set_idempotency_window_size(8);

    std::atomic<int> calls{0};
    m_skeleton->register_method<AddRequest, AddResponse>(kMethod,
        [&](const AddRequest& r) { ++calls; return AddResponse{r.a + r.b}; });
    ASSERT_TRUE(m_skeleton->start());

    std::vector<std::uint8_t> rsp;
    ASSERT_EQ(m_backend->call_method_sync(kSid, kIid, kMethod,
              framed_request(1, AddRequest{1, 1}), rsp, 2000), ComRet::OK);
    ASSERT_EQ(m_backend->call_method_sync(kSid, kIid, kMethod,
              framed_request(2, AddRequest{1, 1}), rsp, 2000), ComRet::OK);

    EXPECT_EQ(calls.load(), 2);
}

// 已知边界：窗口定长，被淘汰的 key 会被当作新请求重新执行
TEST_F(IdempotencyTest, EvictedKeyExecutesAgain) {
    m_skeleton->init();
    m_skeleton->set_rpc_idempotency_enabled(true);
    m_skeleton->set_idempotency_window_size(1);  // 只记最近 1 条

    std::atomic<int> calls{0};
    m_skeleton->register_method<AddRequest, AddResponse>(kMethod,
        [&](const AddRequest& r) { ++calls; return AddResponse{r.a + r.b}; });
    ASSERT_TRUE(m_skeleton->start());

    std::vector<std::uint8_t> rsp;
    ASSERT_EQ(m_backend->call_method_sync(kSid, kIid, kMethod,
              framed_request(1, AddRequest{1, 1}), rsp, 2000), ComRet::OK);
    ASSERT_EQ(m_backend->call_method_sync(kSid, kIid, kMethod,
              framed_request(2, AddRequest{1, 1}), rsp, 2000), ComRet::OK);
    // key=1 已被 key=2 挤出窗口，再来一次会重新执行
    ASSERT_EQ(m_backend->call_method_sync(kSid, kIid, kMethod,
              framed_request(1, AddRequest{1, 1}), rsp, 2000), ComRet::OK);

    EXPECT_EQ(calls.load(), 3) << "窗口外的旧 key 会被重新执行，这是有意的取舍";
}

// =========================================================================
//  客户端：默认关闭、前缀剥离
// =========================================================================

TEST_F(IdempotencyTest, DisabledByDefaultNoKeyPrefix) {
    m_skeleton->init();
    std::atomic<int> calls{0};
    std::int32_t seen_a = -1;
    m_skeleton->register_method<AddRequest, AddResponse>(kMethod,
        [&](const AddRequest& r) { ++calls; seen_a = r.a; return AddResponse{r.a + r.b}; });
    ASSERT_TRUE(m_skeleton->start());

    m_proxy->init();
    ASSERT_TRUE(m_proxy->start());

    const auto rsp = m_proxy->call_method_sync<AddRequest, AddResponse>(kMethod, AddRequest{4, 5});
    EXPECT_EQ(rsp.sum, 9);
    EXPECT_EQ(calls.load(), 1);
    EXPECT_EQ(seen_a, 4) << "未启用时不得插入 key 前缀";
}

TEST_F(IdempotencyTest, EnabledCallStripsKeyBeforeHandler) {
    m_skeleton->init();
    m_skeleton->set_rpc_idempotency_enabled(true);
    m_skeleton->set_idempotency_window_size(8);

    std::atomic<int> calls{0};
    std::int32_t seen_a = -1;
    m_skeleton->register_method<AddRequest, AddResponse>(kMethod,
        [&](const AddRequest& r) { ++calls; seen_a = r.a; return AddResponse{r.a + r.b}; });
    ASSERT_TRUE(m_skeleton->start());

    m_proxy->init();
    m_proxy->set_rpc_idempotency_enabled(true);
    ASSERT_TRUE(m_proxy->start());

    const auto rsp = m_proxy->call_method_sync<AddRequest, AddResponse>(kMethod, AddRequest{6, 7});
    EXPECT_EQ(rsp.sum, 13);
    EXPECT_EQ(calls.load(), 1);
    EXPECT_EQ(seen_a, 6) << "key 前缀必须被剥掉再交给业务 handler";
}

TEST_F(IdempotencyTest, IdempotentCallSucceedsOnHappyPath) {
    m_skeleton->init();
    m_skeleton->set_rpc_idempotency_enabled(true);
    m_skeleton->set_idempotency_window_size(8);

    std::atomic<int> calls{0};
    m_skeleton->register_method<AddRequest, AddResponse>(kMethod,
        [&](const AddRequest& r) { ++calls; return AddResponse{r.a + r.b}; });
    ASSERT_TRUE(m_skeleton->start());

    m_proxy->init();
    m_proxy->set_rpc_idempotency_enabled(true);
    ASSERT_TRUE(m_proxy->start());

    // 一次就通，不应产生额外执行
    const auto rsp = m_proxy->call_method_sync_idempotent<AddRequest, AddResponse>(
        kMethod, AddRequest{20, 22}, 2000, 3);
    EXPECT_EQ(rsp.sum, 42);
    EXPECT_EQ(calls.load(), 1);
}

TEST_F(IdempotencyTest, IdempotentCallFallsBackWhenDisabled) {
    m_skeleton->init();
    std::atomic<int> calls{0};
    m_skeleton->register_method<AddRequest, AddResponse>(kMethod,
        [&](const AddRequest& r) { ++calls; return AddResponse{r.a + r.b}; });
    ASSERT_TRUE(m_skeleton->start());

    m_proxy->init();
    ASSERT_TRUE(m_proxy->start());

    const auto rsp = m_proxy->call_method_sync_idempotent<AddRequest, AddResponse>(
        kMethod, AddRequest{1, 2}, 2000, 3);
    EXPECT_EQ(rsp.sum, 3);
    EXPECT_EQ(calls.load(), 1);
}
