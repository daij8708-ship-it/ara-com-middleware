/**
 * @file    test_fragmentation.cpp
 * @brief   消息分片与重组单元测试。
 */

#include "safety/fragmentation.h"
#include <gtest/gtest.h>

using namespace ara::com::safety;
using ara::com::ComRet;

// =========================================================================
//  基本分片与重组往返
// =========================================================================

TEST(FragmentationTest, SingleFragmentRoundTrip) {
    MessageFragmenter fragmenter(1400);
    MessageReassembler reassembler;

    std::vector<std::uint8_t> original = {0x01, 0x02, 0x03, 0x04, 0x05};

    auto fragments = fragmenter.fragment(original);
    // 负载未超过 MTU，应只有一个分片
    ASSERT_EQ(fragments.size(), 1);

    // Reassemble
    std::vector<std::uint8_t> result;
    reassembler.set_callback([&result](const std::vector<std::uint8_t>& complete) {
        result = complete;
    });

    ComRet ret = reassembler.process_fragment(fragments[0], 1);
    EXPECT_EQ(ret, ComRet::OK);
    EXPECT_EQ(result, original);
}

// =========================================================================
//  Multi-fragment round-trip
// =========================================================================

TEST(FragmentationTest, MultiFragmentRoundTrip) {
    // Use a very small MTU to force fragmentation
    constexpr std::uint16_t SMALL_MTU = 20;  // 20B total, 14B payload per fragment
    MessageFragmenter fragmenter(SMALL_MTU);
    MessageReassembler reassembler;

    // 创建超过单片容量的负载
    std::vector<std::uint8_t> original(100, 0xAB);  // 100 bytes of 0xAB
    for (std::size_t i = 0; i < original.size(); ++i) {
        original[i] = static_cast<std::uint8_t>(i);
    }

    auto fragments = fragmenter.fragment(original);
    EXPECT_GT(fragments.size(), 1);  // Should produce multiple fragments

    // Reassemble
    std::vector<std::uint8_t> result;
    reassembler.set_callback([&result](const std::vector<std::uint8_t>& complete) {
        result = complete;
    });

    for (const auto& frag : fragments) {
        ComRet ret = reassembler.process_fragment(frag, 1);
        EXPECT_EQ(ret, ComRet::OK);
    }

    EXPECT_EQ(result, original);
}

// =========================================================================
//  多发送端分片
// =========================================================================

TEST(FragmentationTest, MultipleSenders) {
    constexpr std::uint16_t SMALL_MTU = 20;
    MessageFragmenter fragmenter(SMALL_MTU);
    MessageReassembler reassembler;

    std::vector<std::uint8_t> data_a(50, 0xAA);
    std::vector<std::uint8_t> data_b(50, 0xBB);

    auto frags_a = fragmenter.fragment(data_a);
    auto frags_b = fragmenter.fragment(data_b);

    std::vector<std::uint8_t> result_a, result_b;
    int count = 0;
    reassembler.set_callback([&](const std::vector<std::uint8_t>& complete) {
        if (count == 0) result_a = complete;
        else result_b = complete;
        count++;
    });

    // 交错输入两个发送端的分片
    for (std::size_t i = 0; i < std::max(frags_a.size(), frags_b.size()); ++i) {
        if (i < frags_a.size()) {
            reassembler.process_fragment(frags_a[i], 1);  // sender_id = 1
        }
        if (i < frags_b.size()) {
            reassembler.process_fragment(frags_b[i], 2);  // sender_id = 2
        }
    }

    EXPECT_EQ(count, 2);
    EXPECT_EQ(result_a, data_a);
    EXPECT_EQ(result_b, data_b);
}

// =========================================================================
//  Invalid fragment handling
// =========================================================================

TEST(FragmentationTest, InvalidFragmentTooShort) {
    MessageReassembler reassembler;

    std::vector<std::uint8_t> too_short = {0x00, 0x01};  // Only 2 bytes, need 6
    ComRet ret = reassembler.process_fragment(too_short, 1);
    EXPECT_EQ(ret, ComRet::FRAGMENT_ERR);
}

TEST(FragmentationTest, InvalidFragmentIndex) {
    MessageReassembler reassembler;

    // Header: msg_id=0, total=2, frag_idx=5 (invalid: >= total), len=0
    std::vector<std::uint8_t> bad = {0x00, 0x00, 0x02, 0x05, 0x00, 0x00};
    ComRet ret = reassembler.process_fragment(bad, 1);
    EXPECT_EQ(ret, ComRet::FRAGMENT_ERR);
}

TEST(FragmentationTest, RejectInvalidPayloadLength) {
    MessageReassembler reassembler;
    std::vector<std::uint8_t> bad = {0x00, 0x01, 0x01, 0x00, 0x00, 0x02, 0xAA};
    EXPECT_EQ(reassembler.process_fragment(bad, 1), ComRet::FRAGMENT_ERR);
}

TEST(FragmentationTest, DuplicateFragmentDoesNotCompleteEarly) {
    MessageFragmenter fragmenter(20);
    MessageReassembler reassembler;
    const std::vector<std::uint8_t> original(40, 0x5A);
    const auto fragments = fragmenter.fragment(original);
    ASSERT_EQ(fragments.size(), 3U);
    bool completed = false;
    reassembler.set_callback([&](const auto&) { completed = true; });
    EXPECT_EQ(reassembler.process_fragment(fragments[0], 1), ComRet::OK);
    EXPECT_EQ(reassembler.process_fragment(fragments[0], 1), ComRet::OK);
    EXPECT_EQ(reassembler.process_fragment(fragments[1], 1), ComRet::OK);
    EXPECT_FALSE(completed);
    EXPECT_EQ(reassembler.process_fragment(fragments[2], 1), ComRet::OK);
    EXPECT_TRUE(completed);
}

TEST(FragmentationTest, SupportsOutOfOrderFragments) {
    MessageFragmenter fragmenter(20);
    MessageReassembler reassembler;
    const std::vector<std::uint8_t> original(40, 0x3C);
    const auto fragments = fragmenter.fragment(original);
    std::vector<std::uint8_t> result;
    reassembler.set_callback([&](const auto& data) { result = data; });
    for (auto it = fragments.rbegin(); it != fragments.rend(); ++it) {
        EXPECT_EQ(reassembler.process_fragment(*it, 1), ComRet::OK);
    }
    EXPECT_EQ(result, original);
}

TEST(FragmentationTest, RejectsTooSmallMtu) {
    EXPECT_THROW(
        (void)MessageFragmenter{
            static_cast<std::uint16_t>(FRAG_HEADER_SIZE)},
        std::invalid_argument);
}

TEST(FragmentationTest, RejectsMoreThan255Fragments) {
    MessageFragmenter fragmenter(7); // 每片只能携带 1 字节
    EXPECT_THROW(fragmenter.fragment(std::vector<std::uint8_t>(256)), std::length_error);
}

// =========================================================================
//  过期缓存清理
// =========================================================================

TEST(FragmentationTest, CleanupStaleBuffer) {
    constexpr std::uint16_t SMALL_MTU = 20;
    MessageFragmenter fragmenter(SMALL_MTU);
    MessageReassembler reassembler;

    std::vector<std::uint8_t> data(100, 0xCC);
    auto fragments = fragmenter.fragment(data);

    // 只发送首个分片
    reassembler.process_fragment(fragments[0], 1);

    // 消息不完整时不应触发回调
    bool callback_triggered = false;
    reassembler.set_callback([&](const std::vector<std::uint8_t>&) {
        callback_triggered = true;
    });

    // 使用短超时清理未完成缓存
    reassembler.cleanup_expired(0);  // Immediately expire

    EXPECT_FALSE(callback_triggered);
}

// =========================================================================
//  大负载
// =========================================================================

TEST(FragmentationTest, LargePayload) {
    MessageFragmenter fragmenter(1400);
    MessageReassembler reassembler;

    // 10 KB 负载
    std::vector<std::uint8_t> large_data(10240);
    for (std::size_t i = 0; i < large_data.size(); ++i) {
        large_data[i] = static_cast<std::uint8_t>(i & 0xFF);
    }

    auto fragments = fragmenter.fragment(large_data);
    EXPECT_GT(fragments.size(), 1);

    std::vector<std::uint8_t> result;
    reassembler.set_callback([&result](const std::vector<std::uint8_t>& complete) {
        result = complete;
    });

    for (const auto& frag : fragments) {
        reassembler.process_fragment(frag, 1);
    }

    EXPECT_EQ(result, large_data);
}
