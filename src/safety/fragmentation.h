/**
 * @file    fragmentation.h
 * @brief   大报文分片与重组。
 *
 * 将超大负载拆分为不超过 MTU 的分片，并在接收端重新组装。
 *
 * Frame format (per fragment):
 *   [msg_id:2B] [total_frags:1B] [frag_index:1B] [payload_len:2B] [payload:N]
 *
 */
#pragma once

#include "common/types.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace ara {
namespace com {
namespace safety {

/// 分片头长度
constexpr std::size_t FRAG_HEADER_SIZE = 6;

/// 默认分片 MTU
constexpr std::uint16_t DEFAULT_MTU = 1400;

/// 单个分片最大负载
constexpr std::uint16_t FRAG_PAYLOAD_SIZE = DEFAULT_MTU - FRAG_HEADER_SIZE;

// =========================================================================
//  Fragmenter (Sender side)
// =========================================================================

/**
 * @class MessageFragmenter
 * @brief 将大消息拆分为不超过 MTU 的分片。
 */
class MessageFragmenter {
public:
    explicit MessageFragmenter(std::uint16_t mtu = DEFAULT_MTU);

    /**
     * @brief 将消息拆分为较小分片。
     * @param data 完整消息负载。
     * @return 包含分片头的分片集合。
     */
    std::vector<std::vector<std::uint8_t>> fragment(const std::vector<std::uint8_t>& data);

private:
    std::uint16_t m_mtu;
    std::uint16_t m_next_msg_id{0};  ///< 用于关联分片的循环消息编号
    std::mutex m_mutex;
};

// =========================================================================
//  Reassembler (Receiver side)
// =========================================================================

/**
 * @class MessageReassembler
 * @brief 将分片重组为完整消息。
 *
 * Maintains a reassembly buffer keyed by (sender_id, msg_id).
 * 所有分片到齐后通过回调交付完整消息。
 */
class MessageReassembler {
public:
    /// 完整消息重组回调
    using ReassemblyCallback = std::function<void(const std::vector<std::uint8_t>& complete_msg)>;

    MessageReassembler() = default;

    /**
     * @brief 设置消息重组完成回调。
     */
    void set_callback(ReassemblyCallback cb);

    /**
     * @brief 向重组器输入收到的分片。
     * @param fragment_data 包含分片头的完整分片。
     * @param sender_id 发送端标识，用于隔离不同来源的缓存。
     * @return 接受分片返回 ComRet::OK，出错返回 FRAGMENT_ERR。
     */
    ComRet process_fragment(const std::vector<std::uint8_t>& fragment_data,
                            std::uint32_t sender_id);

    /// Clean up stale reassembly buffers
    void cleanup_expired(std::uint32_t timeout_ms = 5000);

private:
    struct FragBuffer {
        std::uint8_t total_frags{0};
        std::uint16_t received_count{0};
        std::vector<std::vector<std::uint8_t>> fragments;  // indexed by frag_index
        std::vector<bool> received;
        std::uint64_t last_update_ms{0};
    };

    std::map<std::uint64_t, FragBuffer> m_buffers;  // key = (sender_id << 16) | msg_id
    ReassemblyCallback m_callback;
    std::mutex m_mutex;
};

} // namespace safety
} // namespace com
} // namespace ara
