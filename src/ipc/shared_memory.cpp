/**
 * @file    shared_memory.cpp
 * @brief   IPC 通道原型实现。
 *
 * 当前使用进程内消息队列，仅用于 API 集成测试，不是真实共享内存后端。
 */

#include "shared_memory.h"
#include "common/logger.h"

namespace ara {
namespace com {
namespace ipc {

// 进程内所有通道共享的静态消息队列
std::map<std::string, std::vector<std::vector<std::uint8_t>>> SharedMemoryChannel::s_queues;
std::mutex SharedMemoryChannel::s_queues_mutex;

SharedMemoryChannel::SharedMemoryChannel(const std::string& name)
    : m_name(name) {
    ARA_LOG_INFO("SHM", "SharedMemoryChannel created: " + name);
}

SharedMemoryChannel::~SharedMemoryChannel() {
    if (m_connected) {
        ARA_LOG_DEBUG("SHM", "SharedMemoryChannel destroyed: " + m_name);
    }
}

bool SharedMemoryChannel::init_server() {
    m_connected = true;

    // 确保通道对应的队列存在
    {
        std::lock_guard<std::mutex> lock(s_queues_mutex);
        s_queues[m_name];  // 默认构造空队列
    }

    ARA_LOG_INFO("SHM", "Server channel ready: " + m_name);
    return true;
}

bool SharedMemoryChannel::init_client() {
    m_connected = true;

    // 连接服务端创建的队列
    {
        std::lock_guard<std::mutex> lock(s_queues_mutex);
        if (s_queues.find(m_name) == s_queues.end()) {
            s_queues[m_name];  // 服务端尚未启动时先创建队列
        }
    }

    ARA_LOG_INFO("SHM", "Client channel ready: " + m_name);
    return true;
}

bool SharedMemoryChannel::send(const std::vector<std::uint8_t>& data) {
    if (!m_connected) {
        ARA_LOG_WARN("SHM", "Send on disconnected channel: " + m_name);
        return false;
    }

    if (data.size() > SHM_MAX_MESSAGE_SIZE) {
        ARA_LOG_WARN("SHM", "Message exceeds channel limit: " + m_name);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(s_queues_mutex);
        s_queues[m_name].push_back(data);
    }

    // 已设置接收回调时同步调用；否则客户端可主动轮询 receive()
    if (m_receive_cb) {
        m_receive_cb(data);
    }

    return true;
}

bool SharedMemoryChannel::receive(std::vector<std::uint8_t>& data) {
    if (!m_connected) {
        return false;
    }

    std::lock_guard<std::mutex> lock(s_queues_mutex);

    auto& queue = s_queues[m_name];
    if (queue.empty()) {
        return false;  // No messages available
    }

    data = std::move(queue.front());
    queue.erase(queue.begin());
    return true;
}

bool SharedMemoryChannel::is_connected() const {
    return m_connected;
}

void SharedMemoryChannel::set_receive_callback(ReceiveCallback cb) {
    m_receive_cb = std::move(cb);
}

} // namespace ipc
} // namespace com
} // namespace ara
