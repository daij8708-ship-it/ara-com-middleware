/**
 * @file    types.h
 * @brief   ara::com 风格中间件的通用类型定义。
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>

namespace ara {
namespace com {

// =========================================================================
//  基础标识类型
// =========================================================================

/// 服务标识
using ServiceId    = std::uint16_t;

/// 服务实例标识
using InstanceId   = std::uint16_t;

/// Event Group 标识
using EventGroupId = std::uint16_t;

/// 服务内 Event 标识
using EventId      = std::uint16_t;

/// 服务内 Method 标识
using MethodId     = std::uint16_t;

/// Field 标识
using FieldId      = std::uint16_t;

/// 客户端标识
using ClientId     = std::uint16_t;

/// 用于关联请求与响应的会话标识
using SessionId    = std::uint16_t;

/// 服务接口主版本
using MajorVersion = std::uint8_t;

/// 服务接口次版本
using MinorVersion = std::uint32_t;

// =========================================================================
//  枚举
// =========================================================================

/// 分级日志严重程度
enum class LogLevel : std::uint8_t {
    TRACE = 0,   ///< 详细跟踪
    DEBUG = 1,   ///< 调试信息
    INFO  = 2,   ///< 一般信息
    WARN  = 3,   ///< 警告
    ERROR = 4,   ///< 可恢复的错误
    FATAL = 5    ///< 严重错误级别
};

/// 远端服务可用状态
enum class ServiceAvailability : std::uint8_t {
    UNKNOWN    = 0,  ///< 尚未确定服务状态
    AVAILABLE  = 1,  ///< 服务可用
    UNAVAILABLE = 2  ///< 服务不可用
};

/// Method 调用及内部操作返回码
enum class ComRet : std::uint8_t {
    OK            = 0,   ///< 操作成功
    TIMEOUT       = 1,   ///< 操作超时
    NOT_AVAILABLE = 2,   ///< 服务或端点不可用
    E2E_ERROR     = 3,   ///< E2E 校验失败
    FRAGMENT_ERR  = 4,   ///< 分片或重组出错
    INTERNAL_ERR  = 5,   ///< 框架内部错误
    NOT_CONNECTED = 6    ///< 连接尚未建立
};

// =========================================================================
//  自定义异常类型
// =========================================================================

/// 框架基础异常
class ComException : public std::runtime_error {
public:
    explicit ComException(const std::string& msg)
        : std::runtime_error(msg) {}
};

/// 服务不可用异常
class ServiceNotAvailableException : public ComException {
public:
    explicit ServiceNotAvailableException(const std::string& msg)
        : ComException(msg) {}
};

/// E2E 校验失败异常
class E2EValidationException : public ComException {
public:
    explicit E2EValidationException(const std::string& msg)
        : ComException(msg) {}
};

/// 超时异常
class TimeoutException : public ComException {
public:
    explicit TimeoutException(const std::string& msg)
        : ComException(msg) {}
};

// =========================================================================
//  回调类型别名
// =========================================================================

/// 服务可用性变化回调
using AvailabilityCallback = std::function<void(ServiceId, InstanceId, ServiceAvailability)>;

/// Event 原始负载接收回调
using EventCallback = std::function<void(const std::vector<std::uint8_t>& payload)>;

/// Method 请求处理回调
using MethodHandler = std::function<std::vector<std::uint8_t>(const std::vector<std::uint8_t>& request)>;

/// 异步 Method 结果回调
using MethodReplyCallback = std::function<void(ComRet status, const std::vector<std::uint8_t>& response)>;

// =========================================================================
//  配置结构
// =========================================================================

/// 从 JSON 读取的服务配置
struct ServiceConfig {
    ServiceId    service_id{0};
    InstanceId   instance_id{1};
    MajorVersion major_version{1};
    MinorVersion minor_version{0};
    std::string  ip_address{"127.0.0.1"};  ///< Local bind / remote target IP
    std::uint16_t port{30500};             ///< TCP/UDP port
    std::string  protocol{"UDP"};          ///< "UDP" or "TCP"
    std::string  role{"server"};           ///< "server" or "client"
};

/// E2E 保护配置
struct E2EConfig {
    bool   enabled{true};            ///< 启用 E2E 保护
    std::uint16_t  max_counter{15};  ///< Counter wrap-around value (Profile 1 default: 15)
    /// CRC-8 多项式配置
};

} // namespace com
} // namespace ara
