/**
 * @file    test_resume.cpp
 * @brief   事件断点续收单元测试（NullBackend）。
 *
 * 覆盖：正常往返、断连后回放、缓存溢出报缺口、无重复回放、E2E 覆盖序号，
 * 以及默认关闭的行为。真实跨进程的续收需要 vsomeip（见 docs/run-all-tests.md）。
 */

#include "communication/service_proxy.h"
#include "communication/service_skeleton.h"
#include "communication/runtime.h"
#include "safety/e2e.h"
#include "safety/resume.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <mutex>
#include <vector>

using namespace ara::com;

namespace {

struct SeqEvent {
    std::int32_t value{0};

    std::vector<std::uint8_t> serialize() const {
        return {static_cast<std::uint8_t>((value >> 24) & 0xFF),
                static_cast<std::uint8_t>((value >> 16) & 0xFF),
                static_cast<std::uint8_t>((value >> 8) & 0xFF),
                static_cast<std::uint8_t>(value & 0xFF)};
    }

    static SeqEvent deserialize(const std::vector<std::uint8_t>& data) {
        SeqEvent e;
        if (data.size() >= 4) {
            e.value = (static_cast<std::int32_t>(data[0]) << 24) |
                      (static_cast<std::int32_t>(data[1]) << 16) |
                      (static_cast<std::int32_t>(data[2]) << 8)  |
                       static_cast<std::int32_t>(data[3]);
        }
        return e;
    }
};

} // namespace

class ResumeTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_backend = runtime::create_backend(runtime::BackendType::NULL_BACKEND);
        ServiceConfig server_cfg;
        server_cfg.service_id    = 0x4A4A;
        server_cfg.instance_id   = 1;
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

    // 建一对开启了断点续收的 Skeleton / Proxy
    void start_resume_pair(std::size_t buffer_capacity, bool e2e = true) {
        m_skeleton->init();
        m_skeleton->set_e2e_enabled(e2e);
        m_skeleton->set_resume_enabled(true);
        m_skeleton->set_resume_buffer_size(buffer_capacity);
        m_skeleton->register_event<SeqEvent>(0x0001);
        ASSERT_TRUE(m_skeleton->start());

        m_proxy->init();
        m_proxy->set_e2e_enabled(e2e);
        m_proxy->set_resume_enabled(true);
        ASSERT_TRUE(m_proxy->start());
    }

    std::shared_ptr<runtime::ICommunicationBackend> m_backend;
    ServiceConfig m_client_cfg;
    std::unique_ptr<ServiceSkeleton> m_skeleton;
    std::unique_ptr<ServiceProxy> m_proxy;
};

// 序号头不应破坏正常的事件分发
TEST_F(ResumeTest, SequencedRoundTrip) {
    start_resume_pair(8);

    std::mutex m;
    std::vector<std::int32_t> got;
    m_proxy->subscribe_event<SeqEvent>(0x0001, [&](const SeqEvent& e) {
        std::lock_guard<std::mutex> g(m);
        got.push_back(e.value);
    });

    ASSERT_TRUE(m_skeleton->publish_event(0x0001, SeqEvent{10}));
    ASSERT_TRUE(m_skeleton->publish_event(0x0001, SeqEvent{20}));
    ASSERT_TRUE(m_skeleton->publish_event(0x0001, SeqEvent{30}));

    std::lock_guard<std::mutex> g(m);
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], 10);
    EXPECT_EQ(got[1], 20);
    EXPECT_EQ(got[2], 30);
}

// 客户端断连期间漏掉的事件，重连后应被补收
TEST_F(ResumeTest, ReplayAfterMissedEvents) {
    start_resume_pair(8);

    // 客户端还没订阅：这三条就是"断连期间漏掉的"
    ASSERT_TRUE(m_skeleton->publish_event(0x0001, SeqEvent{1}));
    ASSERT_TRUE(m_skeleton->publish_event(0x0001, SeqEvent{2}));
    ASSERT_TRUE(m_skeleton->publish_event(0x0001, SeqEvent{3}));

    std::mutex m;
    std::vector<std::int32_t> got;
    m_proxy->subscribe_event<SeqEvent>(0x0001, [&](const SeqEvent& e) {
        std::lock_guard<std::mutex> g(m);
        got.push_back(e.value);
    });

    EXPECT_EQ(m_proxy->request_resume(0x0001), safety::ResumeStatus::OK);

    std::lock_guard<std::mutex> g(m);
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], 1);
    EXPECT_EQ(got[1], 2);
    EXPECT_EQ(got[2], 3);
}

// 已被挤出缓存的序号：必须报缺口，而不是假装补齐
TEST_F(ResumeTest, GapWhenBufferEvicted) {
    start_resume_pair(2);  // 只保留最近 2 条

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(m_skeleton->publish_event(0x0001, SeqEvent{i}));
    }

    std::mutex m;
    std::vector<std::int32_t> got;
    m_proxy->subscribe_event<SeqEvent>(0x0001, [&](const SeqEvent& e) {
        std::lock_guard<std::mutex> g(m);
        got.push_back(e.value);
    });

    EXPECT_EQ(m_proxy->request_resume(0x0001), safety::ResumeStatus::GAP);

    std::lock_guard<std::mutex> g(m);
    EXPECT_EQ(got.size(), 2u);       // 只能拿到缓存里还剩的两条
    EXPECT_EQ(got[0], 3);
    EXPECT_EQ(got[1], 4);
}

// 已见过位置的续收：不应产生任何重复投递
// 注意：NullBackend 是同步投递，无法自然产生"实时流与回放重叠"，
// 因此这里只验证"没有漏时不会多投"这一半；去重分支的真重叠场景由真实网络用例覆盖。
TEST_F(ResumeTest, NoSpuriousReplayWhenNothingMissed) {
    start_resume_pair(8);

    std::mutex m;
    std::vector<std::int32_t> got;
    m_proxy->subscribe_event<SeqEvent>(0x0001, [&](const SeqEvent& e) {
        std::lock_guard<std::mutex> g(m);
        got.push_back(e.value);
    });

    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(m_skeleton->publish_event(0x0001, SeqEvent{i}));
    }
    {
        std::lock_guard<std::mutex> g(m);
        ASSERT_EQ(got.size(), 3u);
    }

    EXPECT_EQ(m_proxy->request_resume(0x0001), safety::ResumeStatus::OK);

    std::lock_guard<std::mutex> g(m);
    EXPECT_EQ(got.size(), 3u) << "没有漏事件时续收不应重复投递";
}

// 关掉 E2E 时，序号头仍然单独存在且能正常续收
TEST_F(ResumeTest, SequencedRoundTripWithoutE2E) {
    start_resume_pair(8, /*e2e=*/false);

    ASSERT_TRUE(m_skeleton->publish_event(0x0001, SeqEvent{7}));

    std::mutex m;
    std::vector<std::int32_t> got;
    m_proxy->subscribe_event<SeqEvent>(0x0001, [&](const SeqEvent& e) {
        std::lock_guard<std::mutex> g(m);
        got.push_back(e.value);
    });

    EXPECT_EQ(m_proxy->request_resume(0x0001), safety::ResumeStatus::OK);

    std::lock_guard<std::mutex> g(m);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], 7);
}

// 默认关闭：不加序号头、不注册续收 Method
TEST_F(ResumeTest, DisabledByDefault) {
    m_skeleton->init();
    m_skeleton->register_event<SeqEvent>(0x0001);
    ASSERT_TRUE(m_skeleton->start());

    m_proxy->init();
    ASSERT_TRUE(m_proxy->start());

    std::mutex m;
    int count = 0;
    m_proxy->subscribe_event<SeqEvent>(0x0001, [&](const SeqEvent&) {
        std::lock_guard<std::mutex> g(m);
        ++count;
    });

    ASSERT_TRUE(m_skeleton->publish_event(0x0001, SeqEvent{5}));
    {
        std::lock_guard<std::mutex> g(m);
        ASSERT_EQ(count, 1);
    }

    EXPECT_EQ(m_proxy->request_resume(0x0001), safety::ResumeStatus::GAP);
}

// E2E 保护必须覆盖序号头本身：改序号也要被 CRC 拦下
TEST_F(ResumeTest, E2ECoversSequenceHeader) {
    safety::E2EProtector protector;
    const std::vector<std::uint8_t> business{0xAA, 0xBB, 0xCC};

    std::vector<std::uint8_t> framed = safety::encode_seq(7);
    framed.insert(framed.end(), business.begin(), business.end());
    const auto protected_bytes = protector.protect(framed);

    // 未篡改：校验通过，且能取回原序号与业务负载
    {
        safety::E2EChecker checker;
        std::vector<std::uint8_t> out;
        ASSERT_EQ(checker.check(protected_bytes, out), ComRet::OK);
        EXPECT_EQ(safety::decode_seq(out), 7u);
        EXPECT_EQ(out.size(), safety::RESUME_SEQ_HEADER_SIZE + business.size());
    }

    // 篡改业务负载：CRC 不匹配
    {
        auto tampered = protected_bytes;
        tampered.back() ^= 0xFF;
        safety::E2EChecker checker;
        std::vector<std::uint8_t> out;
        EXPECT_EQ(checker.check(tampered, out), ComRet::E2E_ERROR);
    }

    // 篡改序号头（第 3 字节是序号的首字节，前两字节是 E2E 头）：同样被 CRC 拦下
    {
        auto tampered = protected_bytes;
        tampered[2] ^= 0x01;
        safety::E2EChecker checker;
        std::vector<std::uint8_t> out;
        EXPECT_EQ(checker.check(tampered, out), ComRet::E2E_ERROR);
    }
}
