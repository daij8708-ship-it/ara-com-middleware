/**
 * @file    e2e.h
 * @brief   AUTOSAR Profile 1 风格的端到端数据保护。
 *
 * 提供 CRC-8、计数器管理和数据校验，用于检测消息丢失、重复与篡改。
 *
 * 特性：4位循环计数器、数据与计数器联合 CRC-8、低开销周期信号保护。
 *
 */
#pragma once

#include "common/types.h"

#include <cstdint>
#include <vector>

namespace ara {
namespace com {
namespace safety {

// =========================================================================
//  E2E Profile 1 constants
// =========================================================================

/// E2E header size in bytes: 1 byte CRC + optional counter encoding
constexpr std::size_t E2E_HEADER_SIZE = 2;

/// 计数器回绕前的最大值
constexpr std::uint8_t E2E_MAX_COUNTER = 14;

/// Special counter value indicating a reset
constexpr std::uint8_t E2E_COUNTER_RESET = 15;

// =========================================================================
//  E2E Protector (Sender side)
// =========================================================================

/**
 * @class E2EProtector
 * @brief 为发送消息添加 E2E 保护。
 *
 * 在业务负载前添加由 CRC-8 和计数器组成的 E2E 头。
 */
class E2EProtector {
public:
    explicit E2EProtector(std::uint8_t max_counter = E2E_MAX_COUNTER);

    /**
     * @brief 为数据负载添加 E2E 头。
     * @param data 原始负载字节。
     * @return 添加 E2E 头后的数据。
     */
    std::vector<std::uint8_t> protect(const std::vector<std::uint8_t>& data);

    /// 重置内部计数器，例如重连完成后
    void reset_counter();

private:
    std::uint8_t m_counter{0};
    std::uint8_t m_max_counter{E2E_MAX_COUNTER};
};

// =========================================================================
//  E2E Checker (Receiver side)
// =========================================================================

/**
 * @class E2EChecker
 * @brief 校验接收消息的 E2E 保护。
 *
 * 检查 CRC-8 有效性和计数器推进状态：
 *   - Message corruption (CRC mismatch)
 *   - Message loss (counter gap)
 *   - Message repetition (duplicate counter)
 *   - Message insertion (invalid CRC)
 */
class E2EChecker {
public:
    explicit E2EChecker(std::uint8_t max_counter = E2E_MAX_COUNTER);

    /**
     * @brief 校验消息并移除 E2E 保护头。
     * @param protected_data 带 E2E 头的接收数据。
     * @param[out] out_payload 校验成功后提取出的业务负载。
     * @return 校验成功返回 ComRet::OK，否则返回 E2E_ERROR。
     */
    ComRet check(const std::vector<std::uint8_t>& protected_data,
                 std::vector<std::uint8_t>& out_payload);

    /// Reset checker state (e.g., on connection re-establishment)
    void reset();

private:
    std::uint8_t m_last_counter{0xFF};  // 0xFF = uninitialized
    std::uint8_t m_max_counter{E2E_MAX_COUNTER};
    std::uint32_t m_ok_count{0};
    std::uint32_t m_error_count{0};
};

// =========================================================================
//  底层 CRC-8 计算接口（公开供测试使用）
// =========================================================================

/// 使用多项式 0x1D 计算 CRC-8
std::uint8_t e2e_crc8(const std::vector<std::uint8_t>& data);

/// 使用指定初始值计算 CRC-8
std::uint8_t e2e_crc8_with_initial(std::uint8_t initial, const std::vector<std::uint8_t>& data);

} // namespace safety
} // namespace com
} // namespace ara
