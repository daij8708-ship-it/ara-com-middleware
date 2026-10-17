# 在 vsomeip 上封装一层轻量的 ara::com 风格接口。
cmake_minimum_required(VERSION 3.20)
project(ara-com-middleware VERSION 1.0.0 LANGUAGES CXX)

# ---- C++ 标准 ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- 构建选项 ----
option(ARA_COM_USE_VSOMEIP  "Enable vsomeip backend (requires libvsomeip3)" OFF)
option(ARA_COM_USE_DDS "启用 Fast DDS 2.x 通信后端" OFF)
option(ARA_COM_BUILD_TESTS "Build GTest unit tests" ON)
option(ARA_COM_BUILD_DEMOS "Build demo applications" ON)
option(ARA_COM_BUILD_BENCHMARKS "Build lightweight latency benchmark" ON)
if(UNIX)
    option(ARA_COM_BUILD_SHM "Build POSIX shared-memory IPC channel" ON)
else()
    option(ARA_COM_BUILD_SHM "Build POSIX shared-memory IPC channel" OFF)
endif()
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

if(ARA_COM_USE_DDS)
    find_package(fastrtps 2.5 REQUIRED CONFIG)
    find_package(fastcdr REQUIRED CONFIG)
    if(fastrtps_VERSION VERSION_GREATER_EQUAL 3)
        message(FATAL_ERROR "DDS 后端使用 Fast DDS 2.x API，不支持 3.x")
    endif()
    message(STATUS "Fast DDS 后端已启用：${fastrtps_VERSION}")
endif()

if(ARA_COM_BUILD_TESTS)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        if(ARA_COM_USE_DDS)
            message(FATAL_ERROR "DDS 测试已启用但未找到 GTest；请安装 libgtest-dev 或显式关闭 ARA_COM_BUILD_TESTS")
        endif()
        message(WARNING "未找到 GTest，已跳过单元测试；安装 libgtest-dev 后可启用")
        set(ARA_COM_BUILD_TESTS OFF)
    else()
        message(STATUS "已找到 GTest，启用单元测试")
    endif()
endif()

# ---- 线程库（心跳、重连和日志组件需要）----
find_package(Threads REQUIRED)

# ---- 同机共享内存通道 ----
if(ARA_COM_BUILD_SHM)
    if(NOT UNIX)
        message(FATAL_ERROR "ARA_COM_BUILD_SHM 仅支持 POSIX 平台")
    endif()
    add_compile_definitions(ARA_COM_USE_POSIX_SHM)
    message(STATUS "POSIX shared-memory channel ENABLED")
else()
    message(STATUS "POSIX shared-memory channel DISABLED")
endif()

# ---- 子目录 ----
add_subdirectory(src)

if(ARA_COM_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()

if(ARA_COM_BUILD_DEMOS)
    add_subdirectory(demo)
endif()

if(ARA_COM_BUILD_SHM OR ARA_COM_USE_DDS)
    add_subdirectory(tools)
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

if(ARA_COM_USE_VSOMEIP AND ARA_COM_BUILD_TESTS)
    add_executable(rpc_completion_test tests/rpc_completion_test.cpp)
    target_link_libraries(rpc_completion_test PRIVATE ara_com Threads::Threads)
    target_include_directories(rpc_completion_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()

# 仅为异步 RPC 完成测试开启追踪，普通构建不启用。
option(ARA_COM_RPC_COMPLETION_TRACE "Trace request/session/timeout for RPC completion probe" OFF)
if(ARA_COM_RPC_COMPLETION_TRACE)
    if(NOT ARA_COM_USE_VSOMEIP OR NOT ARA_COM_BUILD_TESTS)
        message(FATAL_ERROR "RPC completion tracing requires vsomeip and ARA_COM_BUILD_TESTS=ON (with GTest available)")
    endif()
    target_compile_definitions(ara_com PRIVATE ARA_COM_RPC_COMPLETION_TRACE)
    target_compile_definitions(rpc_completion_test PRIVATE ARA_COM_RPC_COMPLETION_TRACE)
endif()

# 检查多个在途请求的完成结果，保留原来的单请求测试。
if(ARA_COM_USE_VSOMEIP AND ARA_COM_BUILD_TESTS)
    add_executable(rpc_concurrency_test tests/rpc_concurrency_test.cpp)
    target_link_libraries(rpc_concurrency_test PRIVATE ara_com Threads::Threads)
    target_include_directories(rpc_concurrency_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    if(ARA_COM_RPC_COMPLETION_TRACE)
        target_compile_definitions(rpc_concurrency_test PRIVATE ARA_COM_RPC_COMPLETION_TRACE)
    endif()
endif()

# 服务停止和恢复的集成测试。
if(ARA_COM_USE_VSOMEIP AND ARA_COM_BUILD_TESTS)
    add_executable(rpc_disconnect_recovery_test tests/rpc_disconnect_recovery_test.cpp)
    target_link_libraries(rpc_disconnect_recovery_test PRIVATE ara_com Threads::Threads)
    target_include_directories(rpc_disconnect_recovery_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()

# 使用真实 vsomeip 后端的容量测试，测量时采用 Release 构建。
if(ARA_COM_USE_VSOMEIP AND ARA_COM_BUILD_BENCHMARKS)
    add_executable(vsomeip_capacity bench/vsomeip_capacity.cpp)
    target_link_libraries(vsomeip_capacity PRIVATE ara_com Threads::Threads)
    target_include_directories(vsomeip_capacity PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()
