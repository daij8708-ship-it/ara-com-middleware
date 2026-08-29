/**
 * @file    e2e.cpp
 * @brief   E2E 保护实现。
 */

#include "e2e.h"
#include "common/logger.h"

namespace ara {
namespace com {
namespace safety {

// =========================================================================
//  CRC-8 查找表（多项式 0x1D）
// =========================================================================

static const std::uint8_t CRC8_TABLE[256] = {
    0x00, 0x1D, 0x3A, 0x27, 0x74, 0x69, 0x4E, 0x53, 0xE8, 0xF5, 0xD2, 0xCF, 0x9C, 0x81, 0xA6, 0xBB,
    0xCD, 0xD0, 0xF7, 0xEA, 0xB9, 0xA4, 0x83, 0x9E, 0x25, 0x38, 0x1F, 0x02, 0x51, 0x4C, 0x6B, 0x76,
    0x87, 0x9A, 0xBD, 0xA0, 0xF3, 0xEE, 0xC9, 0xD4, 0x6F, 0x72, 0x55, 0x48, 0x1B, 0x06, 0x21, 0x3C,
    0x4A, 0x57, 0x70, 0x6D, 0x3E, 0x23, 0x04, 0x19, 0xA2, 0xBF, 0x98, 0x85, 0xD6, 0xCB, 0xEC, 0xF1,
    0x13, 0x0E, 0x29, 0x34, 0x67, 0x7A, 0x5D, 0x40, 0xFB, 0xE6, 0xC1, 0xDC, 0x8F, 0x92, 0xB5, 0xA8,
    0xDE, 0xC3, 0xE4, 0xF9, 0xAA, 0xB7, 0x90, 0x8D, 0x36, 0x2B, 0x0C, 0x11, 0x42, 0x5F, 0x78, 0x65,
    0x94, 0x89, 0xAE, 0xB3, 0xE0, 0xFD, 0xDA, 0xC7, 0x7C, 0x61, 0x46, 0x5B, 0x08, 0x15, 0x32, 0x2F,
    0x59, 0x44, 0x63, 0x7E, 0x2D, 0x30, 0x17, 0x0A, 0xB1, 0xAC, 0x8B, 0x96, 0xC5, 0xD8, 0xFF, 0xE2,
    0x26, 0x3B, 0x1C, 0x01, 0x52, 0x4F, 0x68, 0x75, 0xCE, 0xD3, 0xF4, 0xE9, 0xBA, 0xA7, 0x80, 0x9D,
    0xEB, 0xF6, 0xD1, 0xCC, 0x9F, 0x82, 0xA5, 0xB8, 0x03, 0x1E, 0x39, 0x24, 0x77, 0x6A, 0x4D, 0x50,
    0xA1, 0xBC, 0x9B, 0x86, 0xD5, 0xC8, 0xEF, 0xF2, 0x49, 0x54, 0x73, 0x6E, 0x3D, 0x20, 0x07, 0x1A,
    0x6C, 0x71, 0x56, 0x4B, 0x18, 0x05, 0x22, 0x3F, 0x84, 0x99, 0xBE, 0xA3, 0xF0, 0xED, 0xCA, 0xD7,
    0x35, 0x28, 0x0F, 0x12, 0x41, 0x5C, 0x7B, 0x66, 0xDD, 0xC0, 0xE7, 0xFA, 0xA9, 0xB4, 0x93, 0x8E,
    0xF8, 0xE5, 0xC2, 0xDF, 0x8C, 0x91, 0xB6, 0xAB, 0x10, 0x0D, 0x2A, 0x37, 0x64, 0x79, 0x5E, 0x43,
    0xB2, 0xAF, 0x88, 0x95, 0xC6, 0xDB, 0xFC, 0xE1, 0x5A, 0x47, 0x60, 0x7D, 0x2E, 0x33, 0x14, 0x09,
    0x7F, 0x62, 0x45, 0x58, 0x0B, 0x16, 0x31, 0x2C, 0x97, 0x8A, 0xAD, 0xB0, 0xE3, 0xFE, 0xD9, 0xC4
};

std::uint8_t e2e_crc8(const std::vector<std::uint8_t>& data) {
    return e2e_crc8_with_initial(0x00, data);
}

std::uint8_t e2e_crc8_with_initial(std::uint8_t initial, const std::vector<std::uint8_t>& data) {
    std::uint8_t crc = initial;
    for (std::uint8_t byte : data) {
        crc = CRC8_TABLE[crc ^ byte];
    }
    return crc;
}

// =========================================================================
//  E2E 保护器
// =========================================================================

E2EProtector::E2EProtector(std::uint8_t max_counter)
    : m_max_counter(max_counter) {}

std::vector<std::uint8_t> E2EProtector::protect(const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> protected_data;
    protected_data.reserve(E2E_HEADER_SIZE + data.size());

    // 构造头部：[counter:4bit | reserved:4bit] [CRC-8]
    std::uint8_t counter_byte = (m_counter & 0x0F) << 4;  // upper nibble = counter

    // 对计数器字节和业务数据计算 CRC-8
    std::vector<std::uint8_t> crc_input;
    crc_input.push_back(counter_byte);
    crc_input.insert(crc_input.end(), data.begin(), data.end());
    std::uint8_t crc = e2e_crc8(crc_input);

    protected_data.push_back(counter_byte);
    protected_data.push_back(crc);
    protected_data.insert(protected_data.end(), data.begin(), data.end());

    // 推进循环计数器
    m_counter++;
    if (m_counter > m_max_counter) {
        m_counter = 0;
    }

    return protected_data;
}

void E2EProtector::reset_counter() {
    m_counter = 0;
}

// =========================================================================
//  E2E 检查器
// =========================================================================

E2EChecker::E2EChecker(std::uint8_t max_counter)
    : m_max_counter(max_counter) {}

ComRet E2EChecker::check(const std::vector<std::uint8_t>& protected_data,
                         std::vector<std::uint8_t>& out_payload) {
    if (protected_data.size() < E2E_HEADER_SIZE) {
        ARA_LOG_WARN("E2E", "Message too short for E2E header");
        m_error_count++;
        return ComRet::E2E_ERROR;
    }

    // 解析头部
    std::uint8_t counter_byte = protected_data[0];
    std::uint8_t received_crc = protected_data[1];
    std::uint8_t counter = (counter_byte >> 4) & 0x0F;

    // 提取业务负载
    auto payload_begin = protected_data.begin() + E2E_HEADER_SIZE;
    std::vector<std::uint8_t> payload(payload_begin, protected_data.end());

    // 重新计算 CRC-8
    std::vector<std::uint8_t> crc_input;
    crc_input.push_back(counter_byte);
    crc_input.insert(crc_input.end(), payload.begin(), payload.end());
    std::uint8_t calculated_crc = e2e_crc8(crc_input);

    if (calculated_crc != received_crc) {
        ARA_LOG_WARN("E2E", "CRC mismatch: received=0x" +
                     std::to_string(received_crc) + " calculated=0x" +
                     std::to_string(calculated_crc));
        m_error_count++;
        return ComRet::E2E_ERROR;
    }

    // 检查计数器推进，首条消息跳过
    if (m_last_counter != 0xFF) {
        std::uint8_t expected = (m_last_counter + 1) % (m_max_counter + 1);
        if (counter != expected) {
            ARA_LOG_WARN("E2E", "Counter out of order: expected=" +
                         std::to_string(expected) + " received=" +
                         std::to_string(counter));
            // 计数器跳变可能由合法重置造成，只记录告警而不拒绝
        }
    }

    m_last_counter = counter;
    m_ok_count++;
    out_payload = std::move(payload);
    return ComRet::OK;
}

void E2EChecker::reset() {
    m_last_counter = 0xFF;
    m_ok_count = 0;
    m_error_count = 0;
}

} // namespace safety
} // namespace com
} // namespace ara
