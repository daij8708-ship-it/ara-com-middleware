# ---- 库源文件 ----
set(ARA_COM_SOURCES
    # 通用组件
    common/logger.cpp
    common/monitor.cpp
    common/json_parser.cpp

    # 可靠性组件
    safety/e2e.cpp
    safety/fragmentation.cpp
    safety/heartbeat.cpp
    safety/reconnect.cpp
    safety/resume.cpp
    safety/idempotency.cpp

    # 通信层
    communication/runtime.cpp
    communication/shm_backend.cpp
    communication/service_skeleton.cpp
    communication/service_proxy.cpp

    # 同机共享内存通道
    ipc/shared_memory.cpp
)

# ---- 库目标 ----
add_library(ara_com STATIC ${ARA_COM_SOURCES})

target_include_directories(ara_com
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(ara_com
    PUBLIC
        Threads::Threads
)

# 按构建选项链接 vsomeip
if(ARA_COM_USE_VSOMEIP)
    target_link_libraries(ara_com PUBLIC vsomeip3)
endif()

if(ARA_COM_USE_DDS)
    target_sources(ara_com PRIVATE communication/dds_backend.cpp)
    target_link_libraries(ara_com PUBLIC fastrtps fastcdr)
    target_compile_definitions(ara_com PUBLIC ARA_COM_USE_DDS)
endif()

# shm_open / sem_open；glibc 2.34 之后已并入 libc，-lrt 仍然兼容。
if(ARA_COM_BUILD_SHM AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(ara_com PUBLIC rt)
endif()

# 编译器告警
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ara_com PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
    )
elseif(MSVC)
    target_compile_options(ara_com PRIVATE /W4 /wd4100)
endif()

# 库版本属性
set_target_properties(ara_com PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)
