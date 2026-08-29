# =============================================================================
#  ARM64（AArch64）交叉编译工具链
#
#  Ubuntu 22.04 安装命令：
#    sudo apt-get install g++-aarch64-linux-gnu gcc-aarch64-linux-gnu
#
#  使用 Linaro、Yocto SDK 等工具链时，请按实际路径调整编译器配置。
# =============================================================================

# ---- 目标系统 ----
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_CROSSCOMPILING TRUE)

# ---- 编译器 ----
set(CMAKE_C_COMPILER    aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER  aarch64-linux-gnu-g++)

# ---- 编译选项 ----
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -march=armv8-a -mtune=cortex-a72")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a -mtune=cortex-a72")

# ---- Sysroot（按 SDK 调整）----
# 使用自定义 sysroot 时取消下列注释：
# set(CMAKE_SYSROOT /path/to/arm64/sysroot)
# set(CMAKE_FIND_ROOT_PATH /path/to/arm64/sysroot)

# ---- 查找路径：仅搜索目标 sysroot ----
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ---- pkg-config ----
# 交叉编译 vsomeip 时，需确保 sysroot 中存在 ARM64 版本且 pkg-config 可发现。
set(PKG_CONFIG_EXECUTABLE /usr/bin/aarch64-linux-gnu-pkg-config CACHE FILEPATH "pkg-config for ARM64")

message(STATUS "ARM64 cross-compilation toolchain loaded")
message(STATUS "  C compiler:   ${CMAKE_C_COMPILER}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER}")
