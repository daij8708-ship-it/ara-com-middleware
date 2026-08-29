/**
 * @file    fragmentation.cpp
 * @brief   消息分片与重组实现。
 */

#include "fragmentation.h"
#include "common/logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace ara {
namespace com {
namespace safety {

// =========================================================================
//  获取单调时钟毫秒值
// =========================================================================

static std::uint64_t now_ms() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count());
}

// =========================================================================
//  消息分片器
// =========================================================================

MessageFragmenter::MessageFragmenter(std::uint16_t mtu)
    : m_mtu(mtu) {
    if (m_mtu <= FRAG_HEADER_SIZE) {
        throw std::invalid_argument("MTU 必须大于分片头长度");
    }
}

std::vector<std::vector<std::uint8_t>> MessageFragmenter::fragment(
    const std::vector<std::uint8_t>& data) {

    std::vector<std::vector<std::uint8_t>> fragments;

    const std::size_t max_payload = m_mtu - FRAG_HEADER_SIZE;
    const std::size_t required_fragments =
        data.empty() ? 1U : (data.size() + max_payload - 1U) / max_payload;
    if (required_fragments > 255U) {
        throw std::length_error("报文过大：单条报文最多支持 255 个分片");
    }
    const auto total_frags = static_cast<std::uint8_t>(required_fragments);

    std::uint16_t msg_id;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        msg_id = m_next_msg_id++;
    }

    for (std::uint8_t idx = 0; idx < total_frags; ++idx) {
        std::size_t offset = static_cast<std::size_t>(idx) * max_payload;
        std::size_t len = std::min(static_cast<std::size_t>(max_payload),
                                   data.size() - offset);

        std::vector<std::uint8_t> frag;
        frag.reserve(FRAG_HEADER_SIZE + len);

        // 分片头：[msg_id:2B,大端] [total:1B] [index:1B] [length:2B,大端]
        frag.push_back(static_cast<std::uint8_t>((msg_id >> 8) & 0xFF));
        frag.push_back(static_cast<std::uint8_t>(msg_id & 0xFF));
        frag.push_back(total_frags);
        frag.push_back(idx);
        frag.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
        frag.push_back(static_cast<std::uint8_t>(len & 0xFF));

        // 负载
        frag.insert(frag.end(), data.begin() + offset, data.begin() + offset + len);
        fragments.push_back(std::move(frag));
    }

    ARA_LOG_DEBUG("FRAG", "Split " + std::to_string(data.size()) + "B into " +
                  std::to_string(total_frags) + " fragments (msg_id=" +
                  std::to_string(msg_id) + ")");

    return fragments;
}

// =========================================================================
//  消息重组器
// =========================================================================

void MessageReassembler::set_callback(ReassemblyCallback cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callback = std::move(cb);
}

ComRet MessageReassembler::process_fragment(const std::vector<std::uint8_t>& fragment_data,
                                            std::uint32_t sender_id) {
    if (fragment_data.size() < FRAG_HEADER_SIZE) {
        ARA_LOG_WARN("FRAG", "Fragment too small: " + std::to_string(fragment_data.size()) + "B");
        return ComRet::FRAGMENT_ERR;
    }

    // 解析分片头
    std::uint16_t msg_id = (static_cast<std::uint16_t>(fragment_data[0]) << 8) |
                            static_cast<std::uint16_t>(fragment_data[1]);
    std::uint8_t total_frags = fragment_data[2];
    std::uint8_t frag_index  = fragment_data[3];
    std::uint16_t payload_len = (static_cast<std::uint16_t>(fragment_data[4]) << 8) |
                                 static_cast<std::uint16_t>(fragment_data[5]);

    if (total_frags == 0 || frag_index >= total_frags ||
        payload_len != fragment_data.size() - FRAG_HEADER_SIZE) {
        ARA_LOG_WARN("FRAG", "Invalid fragment index: " + std::to_string(frag_index) +
                     "/" + std::to_string(total_frags));
        return ComRet::FRAGMENT_ERR;
    }

    // 组合发送端与消息编号作为缓存键
    std::uint64_t key = (static_cast<std::uint64_t>(sender_id) << 16) | msg_id;

    ReassemblyCallback callback;
    std::vector<std::uint8_t> complete;
    {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& buf = m_buffers[key];
    buf.last_update_ms = now_ms();

    // 首个分片到达时分配缓存
    if (buf.total_frags == 0) {
        buf.total_frags = total_frags;
        buf.fragments.resize(total_frags);
        buf.received.resize(total_frags, false);
    } else if (buf.total_frags != total_frags) {
        m_buffers.erase(key);
        ARA_LOG_WARN("FRAG", "同一消息的分片总数不一致，已丢弃重组缓存");
        return ComRet::FRAGMENT_ERR;
    }

    // 去除头部后保存负载
    if (!buf.received[frag_index]) {
        auto payload_start = fragment_data.begin() + FRAG_HEADER_SIZE;
        buf.fragments[frag_index].assign(payload_start, fragment_data.end());
        buf.received[frag_index] = true;
        buf.received_count++;
    }

    ARA_LOG_TRACE("FRAG", "Fragment " + std::to_string(frag_index + 1) + "/" +
                  std::to_string(total_frags) + " for msg_id=" + std::to_string(msg_id));

    // 检查分片是否全部到齐
    if (buf.received_count >= buf.total_frags) {
        // 按序重组
        for (const auto& frag_payload : buf.fragments) {
            complete.insert(complete.end(), frag_payload.begin(), frag_payload.end());
        }

        ARA_LOG_DEBUG("FRAG", "Reassembled message msg_id=" + std::to_string(msg_id) +
                      " total=" + std::to_string(complete.size()) + "B");

        // Deliver
        callback = m_callback;
        m_buffers.erase(key);
    }
    }

    // 用户回调可能再次进入通信框架，不能在持锁状态下调用。
    if (callback) callback(complete);

    return ComRet::OK;
}

void MessageReassembler::cleanup_expired(std::uint32_t timeout_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::uint64_t now = now_ms();
    auto it = m_buffers.begin();
    while (it != m_buffers.end()) {
        if (now - it->second.last_update_ms > timeout_ms) {
            ARA_LOG_WARN("FRAG", "Dropping incomplete message key=0x" +
                         std::to_string(it->first) + " (received " +
                         std::to_string(it->second.received_count) + "/" +
                         std::to_string(it->second.total_frags) + " fragments)");
            it = m_buffers.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace safety
} // namespace com
} // namespace ara
