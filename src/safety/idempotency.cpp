/**
 * @file    idempotency.cpp
 * @brief   同步 RPC 至多一次语义实现。
 */

#include "idempotency.h"
#include "common/types.h"

namespace ara {
namespace com {
namespace safety {

std::vector<std::uint8_t> encode_request_key(std::uint64_t key) {
    std::vector<std::uint8_t> out;
    out.reserve(REQUEST_KEY_SIZE);
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((key >> shift) & 0xFF));
    }
    return out;
}

std::uint64_t decode_request_key(const std::vector<std::uint8_t>& data) {
    if (data.size() < REQUEST_KEY_SIZE) {
        throw ComException("Idempotent request shorter than the 8-byte request key");
    }
    std::uint64_t key = 0;
    for (std::size_t i = 0; i < REQUEST_KEY_SIZE; ++i) {
        key = (key << 8) | static_cast<std::uint64_t>(data[i]);
    }
    return key;
}

// =========================================================================
//  IdempotencyWindow
// =========================================================================

IdempotencyWindow::IdempotencyWindow(std::size_t capacity)
    : m_capacity(capacity) {}

void IdempotencyWindow::set_capacity(std::size_t capacity) {
    m_capacity = capacity;
    while (m_entries.size() > m_capacity) {
        m_entries.pop_front();
    }
}

bool IdempotencyWindow::lookup(std::uint64_t key, std::vector<std::uint8_t>& out) const {
    for (const auto& entry : m_entries) {
        if (entry.first == key) {
            out = entry.second;
            return true;
        }
    }
    return false;
}

void IdempotencyWindow::record(std::uint64_t key, const std::vector<std::uint8_t>& response) {
    if (m_capacity == 0) return;
    m_entries.emplace_back(key, response);
    while (m_entries.size() > m_capacity) {
        m_entries.pop_front();
    }
}

} // namespace safety
} // namespace com
} // namespace ara
