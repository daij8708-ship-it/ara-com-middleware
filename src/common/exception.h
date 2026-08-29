/**
 * @file    exception.h
 * @brief   框架统一异常与断言宏。
 *
 * 运行时断言失败时抛出类型化异常，避免直接使用会终止进程的 C assert()。
 *
 */
#pragma once

#include "types.h"
#include <sstream>

namespace ara {
namespace com {
namespace internal {

/// 抛出带位置信息的 ComException
inline void throw_com_error(const char* file, int line, const char* func,
                            const std::string& msg) {
    std::ostringstream ss;
    ss << "[" << file << ":" << line << " " << func << "] " << msg;
    throw ComException(ss.str());
}

} // namespace internal
} // namespace com
} // namespace ara

// =========================================================================
//  断言宏
// =========================================================================

/// 条件失败时抛出包含文件、行号和函数信息的异常
#define ARA_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            ::ara::com::internal::throw_com_error(__FILE__, __LINE__, __func__, msg); \
        } \
    } while(0)

/// 支持流式格式化消息的断言
#define ARA_ASSERT_FMT(cond, fmt_msg) \
    do { \
        if (!(cond)) { \
            std::ostringstream _ara_ss; \
            _ara_ss << fmt_msg; \
            ::ara::com::internal::throw_com_error(__FILE__, __LINE__, __func__, _ara_ss.str()); \
        } \
    } while(0)

/// 检查 ComRet，非 OK 时抛出异常
#define ARA_CHECK_COM(ret, msg) \
    do { \
        ::ara::com::ComRet _ara_ret = (ret); \
        if (_ara_ret != ::ara::com::ComRet::OK) { \
            std::ostringstream _ara_ss; \
            _ara_ss << msg << " (code=" << static_cast<int>(_ara_ret) << ")"; \
            ::ara::com::internal::throw_com_error(__FILE__, __LINE__, __func__, _ara_ss.str()); \
        } \
    } while(0)

/// 要求指针非空
#define ARA_REQUIRE_NON_NULL(ptr, msg) \
    ARA_ASSERT((ptr) != nullptr, msg)
