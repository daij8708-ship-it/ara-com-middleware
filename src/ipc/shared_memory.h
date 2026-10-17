/**
 * @file    shared_memory.h
 * @brief   同机共享内存 IPC 通道。
 *
 * 双向通道：服务端到客户端、客户端到服务端各一条 SPSC 环形队列，
 * 队列放在 POSIX 共享内存里，用命名信号量做跨进程唤醒。
 *
 * 通道只负责字节消息的可靠传递，不做服务发现、方法路由或事件订阅；
 * 这些仍由 communication/ 层的 ServiceSkeleton / ServiceProxy 负责。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ara {
namespace com {
namespace ipc {

/// 单条消息的最大长度（与旧接口保持一致）
constexpr std::size_t SHM_MAX_MESSAGE_SIZE = 64 * 1024;  // 64 KB

/// 默认每个方向的环形队列容量
constexpr std::size_t SHM_DEFAULT_CAPACITY = 1024 * 1024;  // 1 MiB

/**
 * @class SharedMemoryChannel
 * @brief 同机双进程字节通道。
 *
 * 服务器必须先调用 init_server() 创建共享内存和信号量；客户端调用
 * init_client() 打开同名对象，服务端尚未启动时会按 connect_timeout_ms 重试。
 * 服务端与客户端的 capacity / max_message 必须一致，否则拒绝连接。
 *
 * 用法：
 * @code
 *   SharedMemoryChannel::Config cfg;
 *   cfg.name = "telemetry";              // 只允许字母、数字、_ - .
 *   SharedMemoryChannel server(cfg);
 *   server.init_server();
 *
 *   SharedMemoryChannel client(cfg);
 *   client.init_client();
 *   client.send(bytes);
 *   server.wait_receive(bytes, 1000);
 * @endcode
 */
class SharedMemoryChannel {
public:
    struct Config {
        std::string name;
        std::size_t capacity{SHM_DEFAULT_CAPACITY};
        std::size_t max_message{SHM_MAX_MESSAGE_SIZE};
        std::uint32_t connect_timeout_ms{5000};
    };

    struct DirectionStats {
        std::uint64_t published{0};
        std::uint64_t consumed{0};
        std::uint64_t dropped{0};
        std::uint64_t queued{0};
    };

    struct Stats {
        DirectionStats outgoing;
        DirectionStats incoming;
    };

    using ReceiveCallback = std::function<void(const std::vector<std::uint8_t>&)>;

    explicit SharedMemoryChannel(Config config);
    explicit SharedMemoryChannel(const std::string& name);
    ~SharedMemoryChannel();

    SharedMemoryChannel(const SharedMemoryChannel&) = delete;
    SharedMemoryChannel& operator=(const SharedMemoryChannel&) = delete;
    SharedMemoryChannel(SharedMemoryChannel&&) = delete;
    SharedMemoryChannel& operator=(SharedMemoryChannel&&) = delete;

    /// 创建共享内存和信号量；同名通道已被存活进程占用时失败。
    bool init_server();

    /// 打开服务端创建的通道；服务端未就绪时按 connect_timeout_ms 重试。
    bool init_client();

    /// 非阻塞发送；队列满或消息超长返回 false，不覆盖未读数据。
    bool send(const std::vector<std::uint8_t>& data);

    /// 非阻塞接收；队列为空返回 false。
    bool receive(std::vector<std::uint8_t>& data);

    /// 等待并接收；timeout_ms 为 0 时等价于 receive()。
    bool wait_receive(std::vector<std::uint8_t>& data, std::uint32_t timeout_ms);

    bool is_connected() const;

    /// 注册接收回调；回调由 start_receiver() 创建的线程调用，且不持内部锁。
    void set_receive_callback(ReceiveCallback callback);

    /// 启动后台接收线程；未设置回调时失败。
    bool start_receiver();

    /// 停止并 join 后台接收线程。
    void stop_receiver();

    Stats stats() const;

    /// 最近一次失败的描述，成功路径不会清空。
    std::string last_error() const;

    const std::string& name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ipc
} // namespace com
} // namespace ara
