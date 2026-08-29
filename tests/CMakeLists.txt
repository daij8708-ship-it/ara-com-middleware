# ---- 测试目标 ----
set(TEST_SOURCES
    test_main.cpp
    test_e2e.cpp
    test_fragmentation.cpp
    test_json_parser.cpp
    test_skeleton.cpp
    test_proxy.cpp
    test_concurrency.cpp
    test_reliability.cpp
)

add_executable(ara_com_tests ${TEST_SOURCES})

target_link_libraries(ara_com_tests
    PRIVATE
        ara_com
        GTest::GTest
        GTest::Main
        Threads::Threads
)

target_include_directories(ara_com_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

# 注册到 CTest
include(GoogleTest)
gtest_discover_tests(ara_com_tests
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "ara-com"
        TIMEOUT 60
)

# 复制测试配置，方便从构建目录直接运行
add_custom_command(TARGET ara_com_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/config/services.json
        $<TARGET_FILE_DIR:ara_com_tests>/services.json
)
