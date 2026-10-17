# ---- 服务端示例 ----
add_executable(demo_server
    demo_server.cpp
)

target_link_libraries(demo_server PRIVATE ara_com Threads::Threads)
target_include_directories(demo_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools)

# ---- 客户端示例 ----
add_executable(demo_client
    demo_client.cpp
)

target_link_libraries(demo_client PRIVATE ara_com Threads::Threads)
target_include_directories(demo_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools)

# ---- 复制配置，方便从构建目录直接运行 ----
add_custom_command(TARGET demo_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/config/services.json
        $<TARGET_FILE_DIR:demo_server>/services.json
)

add_custom_command(TARGET demo_client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/config/services.json
        $<TARGET_FILE_DIR:demo_client>/services.json
)

# ---- 安装 ----
install(TARGETS demo_server demo_client
        RUNTIME DESTINATION bin)
