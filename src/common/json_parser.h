/**
 * @file    json_parser.h
 * @brief   JSON 配置解析模块。
 *
 * 解析服务、E2E、分片、心跳和重连配置。
 *
 */
#pragma once

#include "types.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ara {
namespace com {
namespace config {

/**
 * @class JsonConfigParser
 * @brief 解析 ara::com 风格中间件的 JSON 配置。
 *
 * 配置格式示例：
 * @code
 * {
 *   "services": [
 *     {
 *       "service_id":   1234,
 *       "instance_id":  1,
 *       "major_version": 1,
 *       "minor_version": 0,
 *       "ip_address":   "127.0.0.1",
 *       "port":         30500,
 *       "protocol":     "UDP",
 *       "role":         "server"
 *     },
 *     ...
 *   ],
 *   "e2e": {
 *     "enabled": true,
 *     "max_counter": 15
 *   },
 *   "fragmentation": {
 *     "enabled": true,
 *     "mtu": 1400
 *   },
 *   "heartbeat": {
 *     "enabled": true,
 *     "interval_ms": 1000,
 *     "timeout_ms": 3000
 *   },
 *   "reconnect": {
 *     "enabled": true,
 *     "initial_delay_ms": 100,
 *     "max_delay_ms": 5000,
 *     "backoff_multiplier": 2
 *   }
 * }
 * @endcode
 */
class JsonConfigParser {
public:
    JsonConfigParser() = default;

    /**
     * @brief 加载并解析 JSON 配置文件。
     * @param file_path JSON 文件路径。
     * @return 成功返回 true，解析或 I/O 失败返回 false。
     */
    bool load_file(const std::string& file_path);

    /**
     * @brief 加载并解析 JSON 字符串，主要用于测试。
     * @param json_string JSON 内容。
     * @return 成功返回 true。
     */
    bool load_string(const std::string& json_string);

    /// 获取全部服务配置
    const std::vector<ServiceConfig>& services() const { return m_services; }

    /// 按 service_id 查找配置，未找到时返回 nullptr
    const ServiceConfig* find_service(ServiceId service_id) const;

    /// 获取指定 role 的全部服务
    std::vector<const ServiceConfig*> services_by_role(const std::string& role) const;

    /// 获取 E2E 配置
    const E2EConfig& e2e_config() const { return m_e2e_config; }

    /// 获取分片 MTU
    std::uint16_t fragmentation_mtu() const { return m_fragmentation_mtu; }
    bool fragmentation_enabled() const { return m_fragmentation_enabled; }

    /// 获取心跳参数
    bool heartbeat_enabled() const { return m_heartbeat_enabled; }
    std::uint32_t heartbeat_interval_ms() const { return m_heartbeat_interval_ms; }
    std::uint32_t heartbeat_timeout_ms() const { return m_heartbeat_timeout_ms; }

    /// 获取重连参数
    bool reconnect_enabled() const { return m_reconnect_enabled; }
    std::uint32_t reconnect_initial_delay_ms() const { return m_reconnect_initial_delay_ms; }
    std::uint32_t reconnect_max_delay_ms() const { return m_reconnect_max_delay_ms; }
    std::uint32_t reconnect_backoff_multiplier() const { return m_reconnect_backoff_multiplier; }

    /// 断点续收（默认关闭，需要在配置里显式打开）
    bool resume_enabled() const { return m_resume_enabled; }
    std::size_t resume_buffer_size() const { return m_resume_buffer_size; }

    /// 同步 RPC 至多一次（默认关闭，需要在配置里显式打开）
    bool rpc_idempotency_enabled() const { return m_rpc_idempotency_enabled; }
    std::size_t idempotency_window_size() const { return m_idempotency_window_size; }

private:
    void parse_e2e(const std::string& json_str);
    void parse_globals(const std::string& json_str);

    std::vector<ServiceConfig> m_services;

    // 全局与子系统配置
    E2EConfig     m_e2e_config;
    bool          m_fragmentation_enabled{true};
    std::uint16_t m_fragmentation_mtu{1400};
    bool          m_heartbeat_enabled{true};
    std::uint32_t m_heartbeat_interval_ms{1000};
    std::uint32_t m_heartbeat_timeout_ms{3000};
    bool          m_reconnect_enabled{true};
    std::uint32_t m_reconnect_initial_delay_ms{100};
    std::uint32_t m_reconnect_max_delay_ms{5000};
    std::uint32_t m_reconnect_backoff_multiplier{2};

    // 断点续收：默认关闭；容量缺省 64 条，仅作上限，实际以配置为准
    bool        m_resume_enabled{false};
    std::size_t m_resume_buffer_size{64};

    // 同步 RPC 至多一次：默认关闭
    bool        m_rpc_idempotency_enabled{false};
    std::size_t m_idempotency_window_size{64};
};

} // namespace config
} // namespace com
} // namespace ara
