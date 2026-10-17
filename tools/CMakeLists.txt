add_executable(shm_pair shm_pair.cpp)

target_link_libraries(shm_pair PRIVATE ara_com Threads::Threads)

target_include_directories(shm_pair PRIVATE ${CMAKE_SOURCE_DIR}/src)
