/**
 * @file    test_e2e.cpp
 * @brief   E2E 保护单元测试。
 *
 * Tests CRC-8 calculation, protect/check round-trip,
 * counter progression, CRC mismatch detection, and
 * validation of corrupted data.
 */

#include "safety/e2e.h"
#include <gtest/gtest.h>

using namespace ara::com::safety;
using ara::com::ComRet;

// =========================================================================
//  CRC-8 测试
// =========================================================================

TEST(E2ETest, Crc8KnownVector) {
    // 空数据的 CRC-8 应为 0x00
    std::vector<std::uint8_t> empty;
    EXPECT_EQ(e2e_crc8(empty), 0x00);

    // 单字节 0x00 的 CRC-8 应为 0x00
    std::vector<std::uint8_t> zero = {0x00};
    EXPECT_EQ(e2e_crc8(zero), 0x00);

    // CRC-8 of known pattern
    std::vector<std::uint8_t> data = {0x01, 0x02, 0x03};
    std::uint8_t crc = e2e_crc8(data);
    // CRC 结果应保持确定性
    EXPECT_EQ(e2e_crc8(data), crc);
}

TEST(E2ETest, Crc8WithInitial) {
    std::vector<std::uint8_t> data = {0xAA, 0xBB};
    std::uint8_t crc1 = e2e_crc8(data);
    std::uint8_t crc2 = e2e_crc8_with_initial(0xFF, data);
    EXPECT_NE(crc1, crc2);  // Different initial values -> different CRCs
}

// =========================================================================
//  E2E protect + check round-trip
// =========================================================================

TEST(E2ETest, ProtectAndCheckRoundTrip) {
    E2EProtector protector;
    E2EChecker checker;

    std::vector<std::uint8_t> original = {0x10, 0x20, 0x30, 0x40, 0x50};

    // Protect
    std::vector<std::uint8_t> protected_data = protector.protect(original);

    // Should be 2 bytes larger (E2E header)
    ASSERT_EQ(protected_data.size(), original.size() + E2E_HEADER_SIZE);

    // 校验
    std::vector<std::uint8_t> extracted;
    ComRet ret = checker.check(protected_data, extracted);
    EXPECT_EQ(ret, ComRet::OK);
    EXPECT_EQ(extracted, original);
}

// =========================================================================
//  Corruption detection
// =========================================================================

TEST(E2ETest, DetectCorruption) {
    E2EProtector protector;
    E2EChecker checker;

    std::vector<std::uint8_t> original = {0x01, 0x02, 0x03};
    auto protected_data = protector.protect(original);

    // 篡改一个负载字节
    protected_data[E2E_HEADER_SIZE + 1] ^= 0xFF;

    std::vector<std::uint8_t> extracted;
    ComRet ret = checker.check(protected_data, extracted);
    EXPECT_EQ(ret, ComRet::E2E_ERROR);
}

TEST(E2ETest, DetectTruncation) {
    E2EProtector protector;
    E2EChecker checker;

    std::vector<std::uint8_t> original = {0x01, 0x02, 0x03};
    auto protected_data = protector.protect(original);

    // 截断到只剩头部
    protected_data.resize(E2E_HEADER_SIZE - 1);

    std::vector<std::uint8_t> extracted;
    ComRet ret = checker.check(protected_data, extracted);
    EXPECT_EQ(ret, ComRet::E2E_ERROR);
}

// =========================================================================
//  Counter progression
// =========================================================================

TEST(E2ETest, CounterProgression) {
    E2EProtector protector;
    E2EChecker checker;

    for (int i = 0; i < 20; ++i) {
        std::vector<std::uint8_t> data = {static_cast<std::uint8_t>(i)};
        auto protected_data = protector.protect(data);

        std::vector<std::uint8_t> extracted;
        ComRet ret = checker.check(protected_data, extracted);
        EXPECT_EQ(ret, ComRet::OK);
        EXPECT_EQ(extracted, data);
    }
}

// =========================================================================
//  Counter reset
// =========================================================================

TEST(E2ETest, ResetCounter) {
    E2EProtector protector;

    // Advance counter a few steps
    for (int i = 0; i < 5; ++i) {
        protector.protect({0x00});
    }

    // Reset
    protector.reset_counter();
    auto data = protector.protect({0x42});
    EXPECT_EQ(data[0] & 0xF0, 0x00);  // Counter byte upper nibble should be 0
}

// =========================================================================
//  Empty data
// =========================================================================

TEST(E2ETest, EmptyPayload) {
    E2EProtector protector;
    E2EChecker checker;

    std::vector<std::uint8_t> empty;
    auto protected_data = protector.protect(empty);

    ASSERT_EQ(protected_data.size(), E2E_HEADER_SIZE);

    std::vector<std::uint8_t> extracted;
    ComRet ret = checker.check(protected_data, extracted);
    EXPECT_EQ(ret, ComRet::OK);
    EXPECT_TRUE(extracted.empty());
}
