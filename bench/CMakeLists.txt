# ---- 性能基准程序 ----
add_executable(ara_com_bench
    benchmark.cpp
)

target_link_libraries(ara_com_bench PRIVATE ara_com Threads::Threads)
target_include_directories(ara_com_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 复制配置，方便从构建目录直接运行
add_custom_command(TARGET ara_com_bench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/config/services.json
        $<TARGET_FILE_DIR:ara_com_bench>/services.json
)

install(TARGETS ara_com_bench RUNTIME DESTINATION bin)
