#!/usr/bin/env bash
# =============================================================================
#  ara::com 风格中间件构建脚本
#
#  支持 X86 Debug/Release、ARM64 交叉编译、可选 vsomeip 和单元测试。
#
#  用法：
#    ./build.sh                    # 默认：X86 Debug，不启用 vsomeip，运行测试
#    ./build.sh release            # X86 Release
#    ./build.sh arm64              # ARM64 Debug 交叉编译
#    ./build.sh arm64 release      # ARM64 Release 交叉编译
#    ./build.sh x86 vsomeip        # X86 并启用 vsomeip
#    ./build.sh clean              # 清理构建目录
# =============================================================================

set -euo pipefail

# ---- 构建配置 ----
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
INSTALL_DIR="${PROJECT_DIR}/install"
LOG_FILE="${PROJECT_DIR}/build.log"

# ---- 终端颜色 ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info()  { echo -e "${GREEN}[INFO]${NC}  $*" | tee -a "$LOG_FILE"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $*" | tee -a "$LOG_FILE"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*" | tee -a "$LOG_FILE"; }

# ---- 解析参数 ----
TARGET="x86"
BUILD_TYPE="Debug"
USE_VSOMEIP="OFF"
CLEAN_ONLY=false

for arg in "$@"; do
    case "$arg" in
        x86|x86_64|amd64)
            TARGET="x86"
            ;;
        arm64|aarch64)
            TARGET="arm64"
            ;;
        debug|Debug)
            BUILD_TYPE="Debug"
            ;;
        release|Release)
            BUILD_TYPE="Release"
            ;;
        vsomeip|vsomeip3)
            USE_VSOMEIP="ON"
            ;;
        clean)
            CLEAN_ONLY=true
            ;;
        *)
            log_error "Unknown argument: $arg"
            echo "Usage: $0 [x86|arm64] [debug|release] [vsomeip] [clean]"
            exit 1
            ;;
    esac
done

# ---- 清理 ----
if $CLEAN_ONLY; then
    log_info "Cleaning build directories..."
    rm -rf "$BUILD_DIR" "$INSTALL_DIR"
    rm -f "$LOG_FILE"
    log_info "Clean complete."
    exit 0
fi

# ---- 创建目录 ----
mkdir -p "$BUILD_DIR" "$INSTALL_DIR"

# ---- 初始化构建日志 ----
echo "===== ara::com Build Log — $(date) =====" > "$LOG_FILE"

log_info "=============================================="
log_info "  ara::com Middleware Build"
log_info "=============================================="
log_info "Target:     $TARGET"
log_info "Build type: $BUILD_TYPE"
log_info "Use vsomeip: $USE_VSOMEIP"
log_info "Project:    $PROJECT_DIR"
log_info "=============================================="

# ---- CMake 配置 ----
CMAKE_OPTS=(
    "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
    "-DCMAKE_INSTALL_PREFIX=${INSTALL_DIR}"
    "-DARA_COM_USE_VSOMEIP=${USE_VSOMEIP}"
    "-DARA_COM_BUILD_TESTS=ON"
    "-DARA_COM_BUILD_DEMOS=ON"
)

# 交叉编译工具链
if [ "$TARGET" = "arm64" ]; then
    TOOLCHAIN_FILE="${PROJECT_DIR}/toolchain/arm64-toolchain.cmake"
    if [ ! -f "$TOOLCHAIN_FILE" ]; then
        log_error "ARM64 toolchain file not found: $TOOLCHAIN_FILE"
        log_error "Please edit toolchain/arm64-toolchain.cmake with your cross-compiler paths."
        exit 1
    fi
    CMAKE_OPTS+=("-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE}")
    log_info "Using ARM64 cross-compilation toolchain"
fi

# ---- 生成构建系统 ----
log_info "Configuring CMake..."
cmake -B "$BUILD_DIR" -S "$PROJECT_DIR" "${CMAKE_OPTS[@]}" 2>&1 | tee -a "$LOG_FILE"

if [ ${PIPESTATUS[0]} -ne 0 ]; then
    log_error "CMake configuration failed!"
    exit 1
fi

# ---- 编译 ----
log_info "Building (${BUILD_TYPE})..."
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" -j "$(nproc 2>/dev/null || echo 4)" 2>&1 | tee -a "$LOG_FILE"

if [ ${PIPESTATUS[0]} -ne 0 ]; then
    log_error "Build failed! Check ${LOG_FILE} for details."
    exit 1
fi

log_info "Build succeeded!"

# ---- 运行测试 ----
log_info "Running unit tests..."
cd "$BUILD_DIR"
if ctest --output-on-failure --timeout 60 2>&1 | tee -a "$LOG_FILE"; then
    log_info "All tests passed!"
else
    log_error "Some tests failed!"
    exit 1
fi

# ---- 安装（可选）----
# cmake --install "$BUILD_DIR" --config "$BUILD_TYPE"
# log_info "Installed to: $INSTALL_DIR"

# ---- 构建摘要 ----
log_info "=============================================="
log_info "  Build Summary"
log_info "=============================================="
log_info "Build dir:  $BUILD_DIR"
log_info "Binaries:"
ls -la "$BUILD_DIR"/demo/demo_server "$BUILD_DIR"/demo/demo_client \
      "$BUILD_DIR"/tests/ara_com_tests 2>/dev/null || true
log_info ""
log_info "Run demo server:  $BUILD_DIR/demo/demo_server"
log_info "Run demo client:  $BUILD_DIR/demo/demo_client"
log_info "Run test suite:   cd $BUILD_DIR && ctest --output-on-failure"
log_info "=============================================="

echo ""
echo -e "${GREEN}Build complete!${NC}"
