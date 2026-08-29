# Lightweight ara::com-style API on top of vsomeip.
cmake_minimum_required(VERSION 3.20)
project(ara-com-middleware VERSION 1.0.0 LANGUAGES CXX)

# ---- C++ 标准 ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- 构建选项 ----
option(ARA_COM_USE_VSOMEIP  "Enable vsomeip backend (requires libvsomeip3)" OFF)
option(ARA_COM_BUILD_TESTS "Build GTest unit tests" ON)
option(ARA_COM_BUILD_DEMOS "Build demo applications" ON)
option(ARA_COM_BUILD_BENCHMARKS "Build lightweight latency benchmark" ON)
option(ARA_COM_ENABLE_ASAN "启用 AddressSanitizer" OFF)
option(ARA_COM_ENABLE_UBSAN "启用 UndefinedBehaviorSanitizer" OFF)
option(ARA_COM_ENABLE_TSAN "启用 ThreadSanitizer" OFF)

if(ARA_COM_ENABLE_TSAN AND (ARA_COM_ENABLE_ASAN OR ARA_COM_ENABLE_UBSAN))
    message(FATAL_ERROR "TSan 不能与 ASan/UBSan 同时启用")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    if(ARA_COM_ENABLE_ASAN)
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
    if(ARA_COM_ENABLE_UBSAN)
        add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=undefined)
    endif()
    if(ARA_COM_ENABLE_TSAN)
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    endif()
endif()

# ---- 查找依赖 ----
if(ARA_COM_USE_VSOMEIP)
    find_package(vsomeip3 REQUIRED)
    add_compile_definitions(ARA_COM_USE_VSOMEIP)
    message(STATUS "vsomeip backend ENABLED")
else()
    message(STATUS "vsomeip backend DISABLED — using NullBackend for testing")
endif()

if(ARA_COM_BUILD_TESTS)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        message(WARNING "未找到 GTest，已跳过单元测试；安装 libgtest-dev 后可启用")
        set(ARA_COM_BUILD_TESTS OFF)
    else()
        message(STATUS "已找到 GTest，启用单元测试")
    endif()
endif()

# ---- 线程库（心跳、重连和日志组件需要）----
find_package(Threads REQUIRED)

# ---- 子目录 ----
add_subdirectory(src)

if(ARA_COM_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()

if(ARA_COM_BUILD_DEMOS)
    add_subdirectory(demo)
endif()

if(ARA_COM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- 安装规则 ----
install(DIRECTORY src/common src/communication src/safety src/ipc
        DESTINATION include/ara-com
        FILES_MATCHING PATTERN "*.h")

install(TARGETS ara_com
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib)

install(FILES config/services.json
        DESTINATION etc/ara-com)
