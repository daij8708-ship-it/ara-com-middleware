/**
 * @file    shared_memory.h
 * @brief   同设备 IPC 通道接口原型。
 *
 * 定义双向 IPC 通道接口，并与 ICommunicationBackend 的抽象方向保持一致。
 *
 * 当前仅提供进程内测试替身，不是真实共享内存实现。
 *
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ara {
namespace com {
namespace ipc {

// =========================================================================
//  配置常量
// =========================================================================

/// 最大消息长度
constexpr std::size_t SHM_MAX_MESSAGE_SIZE = 64 * 1024;  // 64 KB

// =========================================================================
//  SharedMemoryChannel
// =========================================================================

/**
 * @class SharedMemoryChannel
 * @brief 双向 IPC 通道接口原型。
 *
 * 当前由进程内消息队列实现，仅供开发与接口测试。
 *
 * 用法：
 * @code
 *   SharedMemoryChannel channel("ara_com_svc_1234");
 *   channel.init_server();  // 或 init_client()
 *   channel.send(data);
 *   channel.receive(data);
 * @endcode
 */
class SharedMemoryChannel {
public:
    explicit SharedMemoryChannel(const std::string& name);
    ~SharedMemoryChannel();

    SharedMemoryChannel(const SharedMemoryChannel&) = delete;
    SharedMemoryChannel& operator=(const SharedMemoryChannel&) = delete;

    /**
     * @brief 初始化服务端通道。
     * @return 成功返回 true。
     */
    bool init_server();

    /**
     * @brief 初始化客户端通道。
     * @return 成功返回 true。
     */
    bool init_client();

    /**
     * @brief 通过通道发送数据。
     * @param data 待发送的原始字节。
     * @return 成功返回 true。
     */
    bool send(const std::vector<std::uint8_t>& data);

    /**
     * @brief 非阻塞接收数据。
     * @param[out] data 接收到的字节。
     * @return 收到数据返回 true，通道为空返回 false。
     */
    bool receive(std::vector<std::uint8_t>& data);

    /// 查询通道是否已连接
    bool is_connected() const;

    /// 注册消息接收回调
    using ReceiveCallback = std::function<void(const std::vector<std::uint8_t>&)>;
    void set_receive_callback(ReceiveCallback cb);

private:
    std::string m_name;
    bool m_connected{false};

    // 进程内测试替身：按通道名保存消息队列
    static std::map<std::string, std::vector<std::vector<std::uint8_t>>> s_queues;
    static std::mutex s_queues_mutex;

    ReceiveCallback m_receive_cb;
};

} // namespace ipc
} // namespace com
} // namespace ara
