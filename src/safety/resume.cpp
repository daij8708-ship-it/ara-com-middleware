/**
 * @file    resume.cpp
 * @brief   事件断点续收实现。
 */

#include "resume.h"

namespace ara {
namespace com {
namespace safety {

namespace {

std::uint32_t read_u32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8)  |
            static_cast<std::uint32_t>(p[3]);
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

} // namespace

std::vector<std::uint8_t> encode_seq(std::uint32_t seq) {
    std::vector<std::uint8_t> out;
    out.reserve(RESUME_SEQ_HEADER_SIZE);
    append_u32(out, seq);
    return out;
}

std::uint32_t decode_seq(const std::vector<std::uint8_t>& data) {
    if (data.size() < RESUME_SEQ_HEADER_SIZE) {
        throw ComException("Sequenced event shorter than the 4-byte sequence header");
    }
    return read_u32(data.data());
}

std::vector<std::uint8_t> encode_resume_request(EventId event_id, std::uint32_t last_seq) {
    std::vector<std::uint8_t> out;
    out.reserve(6);
    out.push_back(static_cast<std::uint8_t>((event_id >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(event_id & 0xFF));
    append_u32(out, last_seq);
    return out;
}

bool decode_resume_request(const std::vector<std::uint8_t>& data,
                           EventId& event_id, std::uint32_t& last_seq) {
    if (data.size() < 6) return false;
    event_id = static_cast<EventId>((static_cast<std::uint16_t>(data[0]) << 8) |
                                     static_cast<std::uint16_t>(data[1]));
    last_seq = read_u32(data.data() + 2);
    return true;
}

std::vector<std::uint8_t> encode_resume_response(ResumeStatus status,
                                                 std::uint16_t count,
                                                 std::uint32_t earliest_seq) {
    std::vector<std::uint8_t> out;
    out.reserve(RESUME_RESPONSE_SIZE);
    out.push_back(static_cast<std::uint8_t>(status));
    out.push_back(static_cast<std::uint8_t>((count >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(count & 0xFF));
    append_u32(out, earliest_seq);
    return out;
}

bool decode_resume_response(const std::vector<std::uint8_t>& data,
                            ResumeStatus& status, std::uint16_t& count,
                            std::uint32_t& earliest_seq) {
    if (data.size() < RESUME_RESPONSE_SIZE) return false;
    status = static_cast<ResumeStatus>(data[0]);
    count = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[1]) << 8) |
                                        static_cast<std::uint16_t>(data[2]));
    earliest_seq = read_u32(data.data() + 3);
    return true;
}

// =========================================================================
//  EventSequenceBuffer
// =========================================================================

EventSequenceBuffer::EventSequenceBuffer(std::size_t capacity)
    : m_capacity(capacity) {}

void EventSequenceBuffer::set_capacity(std::size_t capacity) {
    m_capacity = capacity;
    while (m_entries.size() > m_capacity) {
        m_entries.pop_front();
    }
}

void EventSequenceBuffer::push(std::uint32_t seq, const std::vector<std::uint8_t>& payload) {
    if (m_capacity == 0) return;
    m_entries.emplace_back(seq, payload);
    while (m_entries.size() > m_capacity) {
        m_entries.pop_front();
    }
}

std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>>
EventSequenceBuffer::since(std::uint32_t last_seq) const {
    std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> result;
    for (const auto& entry : m_entries) {
        if (last_seq != RESUME_SEQ_UNKNOWN && entry.first <= last_seq) continue;
        result.push_back(entry);
    }
    return result;
}

bool EventSequenceBuffer::has_gap(std::uint32_t last_seq) const {
    if (last_seq == RESUME_SEQ_UNKNOWN) {
        // 客户端没有任何历史：只要缓存已经从 0 号之后开始，前面的就已经丢了。
        return !m_entries.empty() && m_entries.front().first > 0u;
    }
    if (m_entries.empty()) {
        return true;  // 有明确位置却没有任何历史可回放
    }
    return last_seq + 1u < m_entries.front().first;
}

std::uint32_t EventSequenceBuffer::earliest() const {
    return m_entries.empty() ? RESUME_SEQ_UNKNOWN : m_entries.front().first;
}

} // namespace safety
} // namespace com
} // namespace ara
