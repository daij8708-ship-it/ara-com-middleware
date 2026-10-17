#!/usr/bin/env bash
# =============================================================================
#  ara-com-middleware 一键测试脚本
#
#  在项目根目录执行即可完成：环境检查 → 配置 → 编译 → 运行测试 → 汇总结果。
#  脚本本身不修改任何源码，只使用 CMake 已有的选项和 scripts/ 下已有的脚本。
#
#  用法：
#    bash run-tests.sh                  # 默认：unit + guards + shm + vsomeip（约 4 分钟）
#    bash run-tests.sh quick            # 只跑不依赖 vsomeip 的快速回归（约 1 分钟）
#    bash run-tests.sh all              # unit + guards + shm + asan + tsan + vsomeip
#    bash run-tests.sh shm              # 同机共享内存通道：单测 + 双进程压测
#    bash run-tests.sh vsomeip          # 只跑真实 SOME/IP 端到端（需要 vsomeip3）
#    bash run-tests.sh capacity         # 容量测试（约 21 分钟）
#    bash run-tests.sh unit --clean -j 8
#
#  想把双进程的两侧分开看（服务端 / 客户端各一个终端窗口）：
#    Windows：双击 run-vsomeip.cmd
#    Linux/WSL：bash scripts/vsomeip_pair.sh server [用例] / ... client [用例]
#    Linux/WSL：bash scripts/verify_shm.sh [构建目录]
#
#  模式（可以写多个，按顺序执行）：
#    unit      NullBackend 单元测试（GTest + ctest）
#    guards    测试判定与失败归档的回归检查（python3 + g++）
#    shm       共享内存通道单测 + 双进程吞吐/崩溃恢复验证
#    vsomeip   真实 SOME/IP：双进程链路、异步完成、多在途并发、停止恢复
#    quick     unit + guards（不依赖 vsomeip，用于快速回归）
#    asan      AddressSanitizer + UndefinedBehaviorSanitizer 构建并测试
#    tsan      ThreadSanitizer 构建并测试
#    capacity  vsomeip 容量测试（约 21 分钟）
#    all       unit + guards + shm + asan + tsan + vsomeip
#
#  选项：
#    -j N / -jN       并行编译任务数（默认 nproc）
#    --clean          先删除本次要用到的构建目录
#    --keep-going, -k 某个阶段失败后继续执行后续阶段
#    --no-color       关闭彩色输出（等价于 NO_COLOR=1）
#    --list           列出所有模式
#    -h, --help       显示本帮助
#
#  退出码：0 = 没有失败阶段（可能有个别阶段因缺少依赖被跳过）；1 = 有阶段失败。
# =============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
cd "$SCRIPT_DIR" || exit 2

if [ ! -f "$SCRIPT_DIR/CMakeLists.txt" ]; then
    echo "错误：$SCRIPT_DIR 不是项目根目录（没有找到 CMakeLists.txt）。" >&2
    exit 2
fi

# ---- 运行参数 ----
JOBS="$(nproc 2>/dev/null || echo 4)"
CLEAN=0
KEEP_GOING=0
COLOR_MODE="auto"
MODES=""
GENERATOR=""
ABORTED=0
STAGES=""
CLEANUP_PIDS=""

STAMP="$(date +%Y%m%d-%H%M%S)"
LOG_DIR="$SCRIPT_DIR/artifacts/oneclick-$STAMP"

# ---- 输出颜色 ----
C_RESET=""
C_BOLD=""
C_RED=""
C_GREEN=""
C_YELLOW=""
C_BLUE=""
C_DIM=""

setup_colors() {
    local enabled=1
    local no_color=""
    no_color="$(printenv NO_COLOR 2>/dev/null || true)"
    case "$COLOR_MODE" in
        never) enabled=0 ;;
        always) enabled=1 ;;
        *) if [ -t 1 ] && [ -z "$no_color" ]; then enabled=1; else enabled=0; fi ;;
    esac
    if [ "$enabled" -eq 1 ]; then
        C_RESET=$(printf '\033[0m')
        C_BOLD=$(printf '\033[1m')
        C_RED=$(printf '\033[0;31m')
        C_GREEN=$(printf '\033[0;32m')
        C_YELLOW=$(printf '\033[1;33m')
        C_BLUE=$(printf '\033[0;36m')
        C_DIM=$(printf '\033[2m')
    fi
}

# ---- 依赖探测 ----
have() {
    command -v "$1" >/dev/null 2>&1
}

have_cxx() {
    have c++ || have g++ || have clang++
}

has_gtest() {
    if have pkg-config && pkg-config --exists gtest 2>/dev/null; then
        return 0
    fi
    local f
    for f in /usr/include/gtest/gtest.h \
             /usr/local/include/gtest/gtest.h \
             /usr/lib/x86_64-linux-gnu/cmake/GTest/GTestConfig.cmake \
             /usr/lib/cmake/GTest/GTestConfig.cmake \
             /usr/local/lib/cmake/GTest/GTestConfig.cmake; do
        if [ -e "$f" ]; then
            return 0
        fi
    done
    if find /usr/local /opt -maxdepth 6 -name 'GTestConfig.cmake' -print -quit 2>/dev/null | grep -q .; then
        return 0
    fi
    return 1
}

has_vsomeip() {
    if have pkg-config && pkg-config --exists vsomeip3 2>/dev/null; then
        return 0
    fi
    local d
    for d in /usr/local/lib/cmake/vsomeip3 \
             /usr/lib/x86_64-linux-gnu/cmake/vsomeip3 \
             /usr/lib/cmake/vsomeip3 \
             /opt/vsomeip/lib/cmake/vsomeip3; do
        if [ -d "$d" ]; then
            return 0
        fi
    done
    if find /usr/local /usr/lib /opt -maxdepth 6 -name 'vsomeip3Config.cmake' -print -quit 2>/dev/null | grep -q .; then
        return 0
    fi
    return 1
}

# WSL 的 /mnt/c/... 转成 Windows 形式，方便从 Windows 打开产物目录。
to_windows_path() {
    case "$1" in
        /mnt/*) echo "$1" | sed -e 's|^/mnt/\([a-z]\)/|\1:/|' -e 's|/|\\|g' -e 's|^\([a-z]\)|\U\1|' ;;
        *) echo "" ;;
    esac
}

# ---- 参数解析 ----
usage() {
    sed -n '2,/^# =\{20,\}$/p' "$SCRIPT_DIR/$(basename -- "$0")" |
        sed -e '/^# =\+$/d' -e 's/^# \{0,1\}//' -e 's/^#//'
}

list_modes() {
    cat <<'MODES_EOF'
可用模式：
  unit      NullBackend 单元测试（GTest + ctest）
  guards    测试判定与失败归档的回归检查（python3 + g++）
  shm       共享内存通道单测 + 双进程吞吐/崩溃恢复验证
  vsomeip   真实 SOME/IP：双进程链路、异步完成、多在途并发、停止恢复
  quick     unit + guards（不依赖 vsomeip）
  asan      AddressSanitizer + UndefinedBehaviorSanitizer
  tsan      ThreadSanitizer
  capacity  vsomeip 容量测试（约 21 分钟）
  all       unit + guards + shm + asan + tsan + vsomeip
MODES_EOF
}

add_mode() {
    case " $MODES " in
        *" $1 "*) return 0 ;;
        *) MODES="$MODES $1" ;;
    esac
}

parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            unit|guards|shm|asan|tsan|vsomeip|capacity) add_mode "$1" ;;
            quick)
                add_mode unit
                add_mode guards
                ;;
            all)
                add_mode unit
                add_mode guards
                add_mode shm
                add_mode asan
                add_mode tsan
                add_mode vsomeip
                ;;
            -j|--jobs)
                shift
                if [ $# -eq 0 ]; then
                    echo "错误：-j 需要一个参数。" >&2
                    exit 2
                fi
                JOBS="$1"
                ;;
            -j*) JOBS="$(echo "$1" | sed -e 's/^-j//')" ;;
            --clean) CLEAN=1 ;;
            --keep-going|-k) KEEP_GOING=1 ;;
            --no-color) COLOR_MODE="never" ;;
            --list) list_modes; exit 0 ;;
            -h|--help) usage; exit 0 ;;
            *)
                echo "错误：未知参数 $1" >&2
                echo "" >&2
                usage >&2
                exit 2
                ;;
        esac
        shift
    done
    case "$JOBS" in
        ''|*[!0-9]*)
            echo "错误：并行任务数必须是正整数：$JOBS" >&2
            exit 2
            ;;
        0)
            echo "错误：并行任务数不能是 0。" >&2
            exit 2
            ;;
    esac
}

# ---- 阶段记录 ----
record_stage() {
    printf -v STAGES '%s%s|%s|%s|%s\n' "$STAGES" "$1" "$2" "$3" "$4"
}

run_stage() {
    local name="$1"
    local logfile="$2"
    shift 2
    local start=$SECONDS
    local rc=0
    local elapsed=0
    echo ""
    echo "------------------------------------------------------------"
    echo "$C_BOLD$C_BLUE▶ $name$C_RESET"
    echo "$C_DIM  命令: $*$C_RESET"
    echo "$C_DIM  日志: $logfile$C_RESET"
    "$@" > >(tee "$logfile") 2>&1
    rc=$?
    elapsed=$((SECONDS - start))
    if [ "$rc" -eq 0 ]; then
        echo "$C_GREEN✔ $name 通过（$elapsed s）$C_RESET"
        record_stage PASS "$name" "$elapsed" ""
        return 0
    fi
    echo "$C_RED✘ $name 失败（退出码 $rc，$elapsed s）$C_RESET"
    record_stage FAIL "$name" "$elapsed" "退出码 $rc"
    return "$rc"
}

# 统一的阶段入口：失败且未开启 --keep-going 时中止后续阶段。
stage() {
    if [ "$ABORTED" -eq 1 ]; then
        return 0
    fi
    local name="$1"
    local logfile="$2"
    shift 2
    local rc=0
    run_stage "$name" "$logfile" "$@" || rc=$?
    if [ "$rc" -ne 0 ] && [ "$KEEP_GOING" -eq 0 ]; then
        ABORTED=1
    fi
    return "$rc"
}

skip_stage() {
    echo ""
    echo "$C_YELLOW● $1 跳过：$2$C_RESET"
    record_stage SKIP "$1" "0" "$2"
}

info() {
    echo "$C_DIM$*$C_RESET"
}

# ---- 构建辅助 ----
configure_stage() {
    local display="$1"
    local slug="$2"
    local dir="$3"
    shift 3
    if [ "$CLEAN" -eq 1 ]; then
        info "清理构建目录 $dir"
        rm -rf "$SCRIPT_DIR/$dir"
    fi
    stage "配置 $display" "$LOG_DIR/configure-$slug.log" \
        cmake -S "$SCRIPT_DIR" -B "$SCRIPT_DIR/$dir" $GENERATOR "$@"
}

build_stage() {
    local display="$1"
    local slug="$2"
    local dir="$3"
    stage "编译 $display" "$LOG_DIR/build-$slug.log" \
        cmake --build "$SCRIPT_DIR/$dir" --parallel "$JOBS"
}

run_ctest_stage() {
    local display="$1"
    local slug="$2"
    local dir="$3"
    local rc=0
    stage "$display" "$LOG_DIR/ctest-$slug.log" \
        ctest --test-dir "$SCRIPT_DIR/$dir" --output-on-failure --timeout 90 || rc=$?
    if [ "$rc" -eq 8 ]; then
        echo "$C_YELLOW  提示：ctest 没有发现任何测试，通常是 CMake 没有找到 GTest。$C_RESET"
        echo "$C_YELLOW  Ubuntu/WSL 安装方式：sudo apt-get install libgtest-dev$C_RESET"
    fi
}

# ---- vsomeip 双进程 RPC 用例 ----
# run_rpc_pair <显示名> <目录名> <二进制> <检查器脚本或 -> <客户端超时秒> <客户端参数...>
VSOMEIP_SERVER_APP="ara_com_server_4660"
VSOMEIP_CLIENT_APP="ara_com_client_4660"
VSOMEIP_CFG_REL="config/vsomeip.json"

run_rpc_pair() {
    local display="$1"
    local slug="$2"
    local bin="$3"
    local checker="$4"
    local client_timeout="$5"
    shift 5
    if [ "$ABORTED" -eq 1 ]; then
        return 0
    fi

    local client_args="$*"
    local run_dir="$SCRIPT_DIR/tests/reports/staging/$slug-$STAMP"
    local logfile="$LOG_DIR/$slug.log"
    local start=$SECONDS
    local server_pid=""
    local client_rc=0
    local check_rc=0
    local rc=0
    local reason=""
    local elapsed=0

    mkdir -p "$run_dir"
    echo ""
    echo "------------------------------------------------------------"
    echo "$C_BOLD$C_BLUE▶ $display$C_RESET"
    echo "$C_DIM  证据目录: $run_dir$C_RESET"

    if [ ! -x "$bin" ]; then
        echo "$C_RED✘ $display 失败：找不到可执行文件 $bin$C_RESET"
        record_stage FAIL "$display" "0" "缺少二进制 $bin"
        if [ "$KEEP_GOING" -eq 0 ]; then ABORTED=1; fi
        return 1
    fi

    {
        echo "date=$(date --iso-8601=seconds 2>/dev/null || date)"
        echo "commit=$(git -C "$SCRIPT_DIR" rev-parse HEAD 2>/dev/null || echo uncommitted)"
        echo "run_dir=$run_dir"
        echo "binary=$bin"
        echo "server_app=$VSOMEIP_SERVER_APP"
        echo "client_app=$VSOMEIP_CLIENT_APP"
        echo "vsomeip_configuration=$SCRIPT_DIR/$VSOMEIP_CFG_REL"
        echo "server_command=$bin server"
        echo "client_command=$bin client $client_args"
    } > "$run_dir/command.txt"

    # 服务端同时是 routing manager（config/vsomeip.json 的 routing 字段），必须先启动。
    (
        export VSOMEIP_CONFIGURATION="$SCRIPT_DIR/$VSOMEIP_CFG_REL"
        export VSOMEIP_APPLICATION_NAME="$VSOMEIP_SERVER_APP"
        exec "$bin" server
    ) > "$run_dir/server.stdout.log" 2>&1 &
    server_pid=$!
    CLEANUP_PIDS="$CLEANUP_PIDS $server_pid"

    sleep 2
    if ! kill -0 "$server_pid" 2>/dev/null; then
        echo "$C_RED✘ $display 失败：服务端提前退出，见 $run_dir/server.stdout.log$C_RESET"
        tail -n 20 "$run_dir/server.stdout.log" | tee -a "$logfile"
        record_stage FAIL "$display" "$((SECONDS - start))" "服务端提前退出"
        if [ "$KEEP_GOING" -eq 0 ]; then ABORTED=1; fi
        return 1
    fi

    info "  客户端: $bin client $client_args（超时 $client_timeout s）"
    VSOMEIP_CONFIGURATION="$SCRIPT_DIR/$VSOMEIP_CFG_REL" \
    VSOMEIP_APPLICATION_NAME="$VSOMEIP_CLIENT_APP" \
        timeout --signal=TERM "$client_timeout"s "$bin" client $client_args \
        > >(tee -a "$logfile" "$run_dir/client.stdout.log") 2>&1
    client_rc=$?
    printf 'test_exit=%s\n' "$client_rc" > "$run_dir/test-exit.txt"
    if [ "$client_rc" -ne 0 ]; then
        rc=1
        reason="客户端退出码 $client_rc"
    fi
    if [ "$client_rc" -eq 124 ]; then
        reason="客户端超时（$client_timeout s）"
    fi

    if [ -n "$server_pid" ]; then
        kill -TERM "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
        server_pid=""
        sleep 1
    fi

    if [ "$checker" != "-" ] && [ -s "$run_dir/client.stdout.log" ]; then
        info "  检查器: $checker"
        python3 "$checker" "$run_dir/client.stdout.log" > "$run_dir/check.txt" 2>&1
        check_rc=$?
        cat "$run_dir/check.txt"
        cat "$run_dir/check.txt" >> "$logfile"
        printf 'check_exit=%s\n' "$check_rc" > "$run_dir/check-exit.txt"
        if [ "$check_rc" -ne 0 ]; then
            rc=1
            reason="$reason 检查器退出码 $check_rc"
        fi
    fi

    elapsed=$((SECONDS - start))
    if [ "$rc" -eq 0 ]; then
        echo "$C_GREEN✔ $display 通过（$elapsed s，client=$client_rc check=$check_rc）$C_RESET"
        record_stage PASS "$display" "$elapsed" "client=$client_rc check=$check_rc"
        return 0
    fi
    echo "$C_RED✘ $display 失败：$reason（$elapsed s）$C_RESET"
    record_stage FAIL "$display" "$elapsed" "$reason"
    if [ "$KEEP_GOING" -eq 0 ]; then ABORTED=1; fi
    return 1
}

# ---- 各模式 ----
mode_unit() {
    local display="单元测试 (NullBackend)"
    if ! have cmake; then
        skip_stage "$display" "未找到 cmake（需要 >= 3.20）"
        return 0
    fi
    if ! have_cxx; then
        skip_stage "$display" "未找到 C++ 编译器（g++ / clang++）"
        return 0
    fi
    if ! has_gtest; then
        skip_stage "$display" "未找到 GTest（Ubuntu/WSL: sudo apt-get install libgtest-dev）"
        return 0
    fi
    configure_stage "$display" unit build-unit \
        -DCMAKE_BUILD_TYPE=Release \
        -DARA_COM_USE_VSOMEIP=OFF \
        -DARA_COM_BUILD_SHM=OFF \
        -DARA_COM_BUILD_TESTS=ON \
        -DARA_COM_BUILD_DEMOS=ON \
        -DARA_COM_BUILD_BENCHMARKS=ON
    build_stage "$display" unit build-unit
    run_ctest_stage "运行 $display" unit build-unit
}

mode_guards() {
    local display="判定逻辑回归检查"
    if ! have python3; then
        skip_stage "$display" "未找到 python3"
        return 0
    fi
    if ! have g++; then
        skip_stage "$display" "未找到 g++（测试会即时编译判定条件）"
        return 0
    fi
    stage "$display (tests/test_release_guards.py)" "$LOG_DIR/release-guards.log" \
        python3 "$SCRIPT_DIR/tests/test_release_guards.py" -v
}

mode_asan() {
    local display="ASan/UBSan 单元测试"
    if ! has_gtest; then
        skip_stage "$display" "未找到 GTest（Ubuntu/WSL: sudo apt-get install libgtest-dev）"
        return 0
    fi
    configure_stage "$display" asan build-asan \
        -DCMAKE_BUILD_TYPE=Debug \
        -DARA_COM_USE_VSOMEIP=OFF \
        -DARA_COM_BUILD_TESTS=ON \
        -DARA_COM_ENABLE_ASAN=ON \
        -DARA_COM_ENABLE_UBSAN=ON
    build_stage "$display" asan build-asan
    run_ctest_stage "运行 $display" asan build-asan
}

mode_tsan() {
    local display="TSan 单元测试"
    local rc=0
    local build_rc=0
    local prefix=""
    local arch=""
    if ! has_gtest; then
        skip_stage "$display" "未找到 GTest（Ubuntu/WSL: sudo apt-get install libgtest-dev）"
        return 0
    fi
    # TSan 在新内核上会因 ASLR 熵过高报 "unexpected memory mapping"，
    # 用 setarch -R 对 TSan 的编译与运行关闭 ASLR，不影响其它模式。
    if have setarch; then
        arch="$(uname -m)"
        prefix="setarch $arch -R"
        info "提示：TSan 需要关闭 ASLR 才能在该内核上运行，本模式用 [$prefix] 执行编译和测试。"
    fi
    configure_stage "$display" tsan build-tsan \
        -DCMAKE_BUILD_TYPE=Debug \
        -DARA_COM_USE_VSOMEIP=OFF \
        -DARA_COM_BUILD_TESTS=ON \
        -DARA_COM_ENABLE_TSAN=ON
    stage "编译 $display" "$LOG_DIR/build-tsan.log" \
        $prefix cmake --build "$SCRIPT_DIR/build-tsan" --parallel "$JOBS" || build_rc=$?
    # libtsan 11 不拦截 pthread_cond_clockwait（libstdc++ 的 wait_for 用它），会漏看
    # 等待期间释放的 mutex，把心跳 stop() 的正常加锁误报成 double lock；用 tsan.supp
    # 只抑制这两处，数据竞争检测不受影响。
    stage "运行 $display" "$LOG_DIR/ctest-tsan.log" \
        $prefix env TSAN_OPTIONS="suppressions=$SCRIPT_DIR/tests/tsan.supp" \
        ctest --test-dir "$SCRIPT_DIR/build-tsan" --output-on-failure --timeout 90 || rc=$?
    if [ "$build_rc" -ne 0 ] || [ "$rc" -ne 0 ]; then
        if grep -q "unexpected memory mapping" "$LOG_DIR/ctest-tsan.log" "$LOG_DIR/build-tsan.log" 2>/dev/null; then
            echo "$C_YELLOW  提示：TSan 与内核 ASLR 熵冲突（与项目代码无关），$C_RESET"
            echo "$C_YELLOW  可尝试 sudo sysctl vm.mmap_rnd_bits=28，或安装 util-linux 以使用 setarch -R。$C_RESET"
        fi
    fi
}

mode_shm() {
    local display="共享内存通道"
    if [ "$(uname -s)" != "Linux" ]; then
        skip_stage "$display" "仅支持 Linux/POSIX 共享内存"
        return 0
    fi
    if [ ! -d /dev/shm ]; then
        skip_stage "$display" "/dev/shm 不可用"
        return 0
    fi
    if ! has_gtest; then
        skip_stage "$display" "未找到 GTest（Ubuntu/WSL: sudo apt-get install libgtest-dev）"
        return 0
    fi

    configure_stage "$display 单元测试" shm build-shm \
        -DCMAKE_BUILD_TYPE=Release \
        -DARA_COM_USE_VSOMEIP=OFF \
        -DARA_COM_BUILD_SHM=ON \
        -DARA_COM_BUILD_TESTS=ON \
        -DARA_COM_BUILD_DEMOS=OFF
    build_stage "$display 单元测试" shm build-shm
    stage "运行 $display 单元测试" "$LOG_DIR/ctest-shm.log" \
        ctest --test-dir "$SCRIPT_DIR/build-shm" -R ShmChannelTest \
        --output-on-failure --timeout 120

    stage "双进程共享内存压测" "$LOG_DIR/shm-pair.log" \
        bash "$SCRIPT_DIR/scripts/verify_shm.sh" "$SCRIPT_DIR/build-shm"
}

mode_vsomeip() {
    local display="真实 vsomeip 端到端"
    if ! has_vsomeip; then
        skip_stage "$display" "未找到 vsomeip3（pkg-config 或 CMake 包）"
        return 0
    fi
    if ! has_gtest; then
        info "提示：未找到 GTest，只能执行双进程基础链路，RPC 用例需要 GTest 提供的测试目标。"
    fi

    configure_stage "vsomeip 基础构建" vsomeip-base build \
        -DCMAKE_BUILD_TYPE=Release \
        -DARA_COM_USE_VSOMEIP=ON \
        -DARA_COM_BUILD_TESTS=ON \
        -DARA_COM_BUILD_DEMOS=ON
    build_stage "vsomeip 基础构建" vsomeip-base build
    stage "双进程基础链路 (scripts/verify_vsomeip.sh)" "$LOG_DIR/verify-vsomeip.log" \
        bash "$SCRIPT_DIR/scripts/verify_vsomeip.sh"
    sleep 1
    if [ "$ABORTED" -eq 1 ]; then
        return 0
    fi
    if ! has_gtest; then
        skip_stage "异步完成与多在途 RPC" "未找到 GTest，无法构建 rpc_completion_test / rpc_concurrency_test"
        skip_stage "服务停止与恢复" "未找到 GTest，无法构建 rpc_disconnect_recovery_test"
        return 0
    fi

    configure_stage "RPC 追踪构建" vsomeip-trace build-rpc-trace \
        -DCMAKE_BUILD_TYPE=Release \
        -DARA_COM_USE_VSOMEIP=ON \
        -DARA_COM_BUILD_TESTS=ON \
        -DARA_COM_RPC_COMPLETION_TRACE=ON
    build_stage "RPC 追踪构建" vsomeip-trace build-rpc-trace
    run_rpc_pair "异步 RPC 完成（100 次）" rpc-completion \
        "$SCRIPT_DIR/build-rpc-trace/rpc_completion_test" \
        "$SCRIPT_DIR/scripts/check_rpc_completion_trace.py" 180 100
    sleep 1
    run_rpc_pair "多在途并发（窗口 4，100 次）" rpc-concurrency-w4 \
        "$SCRIPT_DIR/build-rpc-trace/rpc_concurrency_test" \
        "$SCRIPT_DIR/scripts/check_rpc_concurrency_trace.py" 180 100 4
    sleep 1
    run_rpc_pair "多在途并发（窗口 16，1000 次）" rpc-concurrency-w16 \
        "$SCRIPT_DIR/build-rpc-trace/rpc_concurrency_test" \
        "$SCRIPT_DIR/scripts/check_rpc_concurrency_trace.py" 180 1000 16
    sleep 1

    configure_stage "停止恢复构建" vsomeip-disconnect build-rpc-disconnect \
        -DCMAKE_BUILD_TYPE=Release \
        -DARA_COM_USE_VSOMEIP=ON \
        -DARA_COM_BUILD_TESTS=ON \
        -DARA_COM_RPC_COMPLETION_TRACE=OFF
    build_stage "停止恢复构建" vsomeip-disconnect build-rpc-disconnect
    stage "服务停止与恢复 (scripts/run_rpc_disconnect_recovery.sh)" "$LOG_DIR/rpc-disconnect.log" \
        bash "$SCRIPT_DIR/scripts/run_rpc_disconnect_recovery.sh"
}

mode_capacity() {
    local display="vsomeip 容量测试（约 21 分钟）"
    if ! has_vsomeip; then
        skip_stage "$display" "未找到 vsomeip3（pkg-config 或 CMake 包）"
        return 0
    fi
    if ! have python3; then
        skip_stage "$display" "未找到 python3（离线检查器需要）"
        return 0
    fi
    if [ ! -x /usr/bin/time ]; then
        skip_stage "$display" "未找到 GNU time（sudo apt-get install time）"
        return 0
    fi
    configure_stage "容量测试构建" capacity build-vsomeip-capacity \
        -DCMAKE_BUILD_TYPE=Release \
        -DARA_COM_USE_VSOMEIP=ON \
        -DARA_COM_BUILD_BENCHMARKS=ON \
        -DARA_COM_RPC_COMPLETION_TRACE=OFF
    build_stage "容量测试构建" capacity build-vsomeip-capacity
    stage "$display (scripts/run_vsomeip_capacity.sh)" "$LOG_DIR/capacity.log" \
        bash "$SCRIPT_DIR/scripts/run_vsomeip_capacity.sh"
}

# ---- 环境与汇总 ----
print_environment() {
    echo "============================================================"
    echo "$C_BOLD  ara-com-middleware 一键测试$C_RESET"
    echo "============================================================"
    echo "  项目目录: $SCRIPT_DIR"
    echo "  运行环境: $(uname -srmo 2>/dev/null || uname -a)"
    if [ -f /etc/os-release ]; then
        echo "  发行版:   $(. /etc/os-release; echo "$PRETTY_NAME")"
    fi
    echo "  CMake:    $(cmake --version 2>/dev/null | head -n 1 || echo 未安装)"
    echo "  编译器:   $((c++ --version 2>/dev/null || g++ --version 2>/dev/null || echo 未找到) | head -n 1)"
    echo "  并行任务: $JOBS"
    echo "  执行模式:$MODES"
    echo "  日志目录: $LOG_DIR"
    case "$SCRIPT_DIR" in
        /mnt/?/*)
            echo "  $C_YELLOW注意: 项目位于 Windows 挂载盘（/mnt/...），构建与计时类测试会比 Linux 原生文件系统慢。$C_RESET"
            ;;
    esac
}

print_summary() {
    local pass=0
    local fail=0
    local skip=0
    local status=""
    local name=""
    local seconds=""
    local note=""
    local tag=""
    local color=""
    local win=""
    echo ""
    echo "============================================================"
    echo "$C_BOLD  测试汇总$C_RESET"
    echo "============================================================"
    while IFS='|' read -r status name seconds note; do
        [ -n "$status" ] || continue
        case "$status" in
            PASS)
                color="$C_GREEN"
                tag="通过"
                pass=$((pass + 1))
                ;;
            FAIL)
                color="$C_RED"
                tag="失败"
                fail=$((fail + 1))
                ;;
            *)
                color="$C_YELLOW"
                tag="跳过"
                skip=$((skip + 1))
                ;;
        esac
        if [ -n "$note" ]; then
            echo "  $color[$tag]$C_RESET $name — $note"
        else
            echo "  $color[$tag]$C_RESET $name （$seconds s）"
        fi
    done <<< "$STAGES"
    echo "------------------------------------------------------------"
    echo "  通过 $pass 项，失败 $fail 项，跳过 $skip 项"
    echo "  日志目录: $LOG_DIR"
    win="$(to_windows_path "$LOG_DIR")"
    if [ -n "$win" ]; then
        echo "  Windows 路径: $win"
    fi
    if [ "$ABORTED" -eq 1 ]; then
        echo "  $C_YELLOW提示: 第一个失败阶段之后已中止后续阶段，可以用 --keep-going 跑完所有阶段。$C_RESET"
    fi
    case " $MODES " in
        *" vsomeip "*|*" capacity "*) ;;
        *) echo "  $C_DIM提示: 本次未包含真实 vsomeip 端到端测试，可用 bash run-tests.sh all 运行完整套件。$C_RESET" ;;
    esac
    if [ "$fail" -gt 0 ]; then
        echo "  $C_RED结论: 存在失败阶段，请查看上面的日志文件。$C_RESET"
        return 1
    fi
    if [ "$skip" -gt 0 ]; then
        echo "  $C_YELLOW结论: 没有失败阶段，但有 $skip 项被跳过（缺少依赖或未选择模式）。$C_RESET"
        return 0
    fi
    echo "  $C_GREEN结论: 全部通过。$C_RESET"
    return 0
}

cleanup() {
    local p=""
    local killed=0
    for p in $CLEANUP_PIDS; do
        if kill -0 "$p" 2>/dev/null; then
            kill -TERM "$p" 2>/dev/null || true
            killed=1
        fi
    done
    if [ "$killed" -eq 1 ]; then
        sleep 1
        for p in $CLEANUP_PIDS; do
            if kill -0 "$p" 2>/dev/null; then
                kill -KILL "$p" 2>/dev/null || true
            fi
        done
    fi
}

main() {
    parse_args "$@"
    if [ -z "$MODES" ]; then
        MODES=" unit guards shm vsomeip"
    fi
    if have ninja; then
        GENERATOR="-G Ninja"
    fi
    mkdir -p "$LOG_DIR"
    print_environment
    local m=""
    for m in $MODES; do
        case "$m" in
            unit) mode_unit ;;
            guards) mode_guards ;;
            shm) mode_shm ;;
            asan) mode_asan ;;
            tsan) mode_tsan ;;
            vsomeip) mode_vsomeip ;;
            capacity) mode_capacity ;;
        esac
    done
    local rc=0
    print_summary || rc=$?
    echo "$LOG_DIR" > "$SCRIPT_DIR/artifacts/last-run.txt"
    return "$rc"
}

setup_colors
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM
main "$@"
exit $?
