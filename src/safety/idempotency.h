/**
 * @file    idempotency.h
 * @brief   同步 RPC 的"至多一次"语义（项目自定义扩展，非标准 ara::com / SOME/IP）。
 *
 * 同步调用超时是二义的：TIMEOUT 不代表服务端没有执行。调用方若要重试，就必须有
 * 去重，否则同一命令会执行两次。这一层给出：
 *   - 客户端为每次逻辑调用生成一个 64 位请求 key，重试时复用同一个 key；
 *   - 服务端按 key 维护定长环形窗口，命中直接返回缓存响应、不重复执行 handler。
 *
 * 已知边界（有意为之，不是遗漏）：
 *   - 窗口是内存态且定长：超出窗口的旧 key 会被当作新请求重新执行；
 *   - 不做在途互斥：同一 key 的并发请求可能都被执行（覆盖场景是"超时后顺序重试"）；
 *   - 服务端重启窗口即失效。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace ara {
namespace com {
namespace safety {

/// 请求 key 长度（64 位，大端）
constexpr std::size_t REQUEST_KEY_SIZE = 8;

/// 把 64 位请求 key 编码为 8 字节大端
std::vector<std::uint8_t> encode_request_key(std::uint64_t key);

/// 从字节流头部读取 64 位大端 key；长度不足时抛出 ComException
std::uint64_t decode_request_key(const std::vector<std::uint8_t>& data);

/**
 * @class IdempotencyWindow
 * @brief 单个方法的定长去重窗口：key -> 已产生的响应。
 *
 * 命中即返回缓存响应，未命中由调用方执行后写入。容量为 0 表示完全不记录。
 */
class IdempotencyWindow {
public:
    explicit IdempotencyWindow(std::size_t capacity = 0);

    void set_capacity(std::size_t capacity);

    /// 命中则把缓存响应写入 out 并返回 true
    bool lookup(std::uint64_t key, std::vector<std::uint8_t>& out) const;

    /// 记录一次执行结果；超出容量时丢弃最早的
    void record(std::uint64_t key, const std::vector<std::uint8_t>& response);

    bool empty() const { return m_entries.empty(); }
    std::size_t size() const { return m_entries.size(); }
    std::size_t capacity() const { return m_capacity; }

private:
    std::deque<std::pair<std::uint64_t, std::vector<std::uint8_t>>> m_entries;
    std::size_t m_capacity{0};
};

} // namespace safety
} // namespace com
} // namespace ara
