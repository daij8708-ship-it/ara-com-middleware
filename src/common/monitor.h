/**
 * @file    monitor.h
 * @brief   进程内运行监测与诊断指标。
 *
 * 所有计数使用原子变量，可在通信线程、业务线程和定时线程中并发更新。
 * 快照写入采用"临时文件 + 原子改名"，Web 面板可随时读取而不必加锁。
 */
#pragma once

#include "common/types.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace ara {
namespace com {
namespace monitor {

/// 可观测指标。新增项必须同步更新 kCounterNames 和 Count。
enum class Counter : std::size_t {
    RpcSubmitted = 0,      ///< 已提交的 Method 调用（同步与异步各算一次）
    RpcOk,                 ///< 返回 OK
    RpcTimeout,            ///< 返回 TIMEOUT
    RpcUnavailable,        ///< NOT_AVAILABLE 或 NOT_CONNECTED
    RpcFailed,             ///< 其余错误
    RpcRequestBytes,       ///< 请求负载字节数
    RpcResponseBytes,      ///< 响应负载字节数
    RpcLatencyUsTotal,     ///< 完成调用的总延迟
    RpcLatencySamples,     ///< 完成调用数（用于计算平均延迟）
    RpcLatencyMaxUs,       ///< 最大单次延迟
    PendingCalls,          ///< 当前在途调用（仪表值）

    EventsSent,            ///< 成功发出的 Event 条数
    EventsSendFailed,      ///< 发送失败的 Event 条数
    EventsSentBytes,       ///< 发出的业务负载字节数
    EventsReceived,        ///< 从传输层收到的 Event 条数
    EventsReceivedBytes,   ///< 收到的传输层字节数
    EventsDelivered,       ///< 通过校验并交付给业务回调的 Event
    EventsDuplicate,       ///< 序号重复被丢弃的 Event
    EventsCorrupt,         ///< E2E 失败、分片错误或格式非法的 Event
    EventSeqGaps,          ///< 检测到的序号跳变次数
    EventSeqMissing,       ///< 按序号推算出的缺失 Event 条数

    ResumeRequests,        ///< 发起的续收请求（含自动续收）
    ResumeReplayed,        ///< 续收回放的事件条数
    ResumeGaps,            ///< 服务端无法补齐的缺口次数
    ResumeFailures,        ///< 续收请求失败或响应非法

    Reconnects,            ///< 不可用恢复为可用的次数
    Disconnects,           ///< 可用变为不可用的次数
    HeartbeatTimeouts,     ///< 心跳超时次数

    MethodsHandled,        ///< 服务端执行的 Method handler 次数
    MethodFailures,        ///< Method handler 抛异常的次数
    IdempotencyHits,       ///< 命中至多一次去重窗口的次数

    ServiceAvailable,      ///< 服务可用性仪表值：1 可用，0 不可用

    Count
};

/// 指标名（用于快照 JSON 和面板展示）
const char* counter_name(Counter counter);

/// 错误码名与中文说明
const char* com_ret_name(ComRet code);
const char* com_ret_description(ComRet code);

/// 进程级标签；同一进程内的 Proxy/Skeleton 共享一组指标
struct ProcessInfo {
    std::string role{"unknown"};
    std::string backend{"unknown"};
    ServiceId   service_id{0};
    InstanceId  instance_id{0};
};

/**
 * @class Monitor
 * @brief 进程级指标单例，并负责周期写入 JSON 快照。
 *
 * 默认不写文件；只有显式调用 start_snapshot_writer() 才启动后台线程。
 */
class Monitor {
public:
    static Monitor& instance();

    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    /// 设置进程标签；应在启动通信前调用
    void configure(ProcessInfo info);

    /// 累加计数
    void inc(Counter counter, std::uint64_t delta = 1);

    /// 写入仪表值
    void set(Counter counter, std::uint64_t value);

    /// 读取当前值
    std::uint64_t get(Counter counter) const;

    /// 记录一次 RPC 完成：更新结果分类、延迟、字节数和在途数
    void complete_rpc(ComRet status, std::uint64_t latency_us,
                      std::size_t response_bytes);

    /// 标记进程已结束，最后一次快照会带上 stopped=true
    void mark_stopped() { m_stopped.store(true); }

    /// 进程是否已标记结束
    bool stopped() const { return m_stopped.load(); }

    /// 生成快照 JSON（单行）
    std::string snapshot_json() const;

    /**
     * @brief 启动周期快照线程。
     * @param directory 输出目录，文件名固定为 metrics-<role>-<pid>.json
     * @param interval_ms 写盘周期，最小 100 ms
     * @return 目录创建失败或已在运行时返回 false
     */
    bool start_snapshot_writer(const std::string& directory,
                               std::uint32_t interval_ms = 500);

    /// 停止快照线程并写最后一次快照
    void stop_snapshot_writer();

    /// 快照文件路径；未启动写入时为空
    std::string snapshot_path() const;

    ~Monitor();

private:
    Monitor();

    static constexpr std::size_t index(Counter counter) {
        return static_cast<std::size_t>(counter);
    }

    void write_snapshot_once() const;

    mutable std::mutex    m_info_mutex;
    ProcessInfo           m_info;
    std::chrono::system_clock::time_point m_started_at =
        std::chrono::system_clock::now();
    std::array<std::atomic<std::uint64_t>,
               static_cast<std::size_t>(Counter::Count)> m_counters{};
    std::atomic<bool>       m_stopped{false};

    std::thread             m_writer_thread;
    bool                    m_writer_starting{false};
    std::atomic<bool>       m_writer_stop{false};
    mutable std::mutex      m_writer_mutex;
    std::condition_variable m_writer_cv;
    std::string             m_snapshot_directory;
    std::string             m_snapshot_path;
    std::uint32_t           m_snapshot_interval_ms{500};
};

/// 便捷入口
inline Monitor& monitor() { return Monitor::instance(); }

} // namespace monitor
} // namespace com
} // namespace ara
