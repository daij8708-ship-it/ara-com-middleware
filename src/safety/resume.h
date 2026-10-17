/**
 * @file    resume.h
 * @brief   事件的断点续收（项目自定义扩展，非标准 ara::com / SOME/IP）。
 *
 * 为 Event 增加一根 32 位单调序号、发布端环形缓存，以及一个以保留 Method 形式
 * 承载的"续收请求"。职责划分：
 *   - E2E 负责"收到的这条是否完整、是否被篡改"；
 *   - 序号负责"流位置"，用来发现缺口并请求回放。
 * 二者不互相替代：E2E 计数器只有 4 位且重连时会被清零，不能当续收游标。
 */
#pragma once

#include "common/types.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace ara {
namespace com {
namespace safety {

/// 事件序号头长度（32 位，大端）
constexpr std::size_t RESUME_SEQ_HEADER_SIZE = 4;

/// 保留的续收 Method 标识。Method 最高位为 0，与 Event 约定区分开。
constexpr MethodId RESUME_METHOD_ID = 0x7FFE;

/// 客户端还没有任何已见序号时使用，表示"把缓存里现存的全发来"
constexpr std::uint32_t RESUME_SEQ_UNKNOWN = 0xFFFFFFFFu;

/// resume 响应固定长度：[status:1B][count:2B][earliest:4B]
constexpr std::size_t RESUME_RESPONSE_SIZE = 7;

/// 续收结果
enum class ResumeStatus : std::uint8_t {
    OK  = 0,  ///< 已按请求回放（可能为 0 条）
    GAP = 1   ///< 请求位置已早于缓存范围，中间缺口无法补齐
};

/// 把 32 位序号编码为 4 字节大端
std::vector<std::uint8_t> encode_seq(std::uint32_t seq);

/// 从字节流头部读取大端 32 位序号；长度不足时抛出 ComException
std::uint32_t decode_seq(const std::vector<std::uint8_t>& data);

/// 拼 resume 请求负载：[event_id:2B][last_seq:4B]
std::vector<std::uint8_t> encode_resume_request(EventId event_id, std::uint32_t last_seq);

/// 解析 resume 请求负载；成功返回 true
bool decode_resume_request(const std::vector<std::uint8_t>& data,
                           EventId& event_id, std::uint32_t& last_seq);

/// 拼 resume 响应负载：[status:1B][count:2B][earliest:4B]
std::vector<std::uint8_t> encode_resume_response(ResumeStatus status,
                                                 std::uint16_t count,
                                                 std::uint32_t earliest_seq);

/// 解析 resume 响应负载；成功返回 true
bool decode_resume_response(const std::vector<std::uint8_t>& data,
                            ResumeStatus& status, std::uint16_t& count,
                            std::uint32_t& earliest_seq);

/**
 * @class EventSequenceBuffer
 * @brief 单个 Event 的定长环形缓存，保存 (序号, 业务负载)。
 *
 * 只保留最近 capacity 条；被挤出的最早序号一旦超过请求位置，就构成无法补齐的
 * 缺口——这时续收要明确报 GAP，而不是假装补齐。
 */
class EventSequenceBuffer {
public:
    explicit EventSequenceBuffer(std::size_t capacity = 0);

    /// 调整容量；容量为 0 表示不缓存
    void set_capacity(std::size_t capacity);

    /// 追加一条；超容量时丢弃最旧的
    void push(std::uint32_t seq, const std::vector<std::uint8_t>& payload);

    /// 取序号严格大于 last_seq 的全部条目；last_seq == UNKNOWN 时取全部
    std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>>
    since(std::uint32_t last_seq) const;

    /// 以 last_seq 为起点续收时，中间是否已经有被丢弃的缺口
    bool has_gap(std::uint32_t last_seq) const;

    bool empty() const { return m_entries.empty(); }
    std::size_t size() const { return m_entries.size(); }
    std::size_t capacity() const { return m_capacity; }

    /// 缓存中最小序号；为空时返回 RESUME_SEQ_UNKNOWN
    std::uint32_t earliest() const;

private:
    std::deque<std::pair<std::uint32_t, std::vector<std::uint8_t>>> m_entries;
    std::size_t m_capacity{0};
};

} // namespace safety
} // namespace com
} // namespace ara
