#!/usr/bin/env bash
# =============================================================================
#  vsomeip 双进程用例的"单侧"启动脚本：服务端和客户端各开一个终端
#
#  它只负责一侧：设置 VSOMEIP_CONFIGURATION / VSOMEIP_APPLICATION_NAME、
#  启停对应的二进制、把本侧日志单独落盘。两侧通过一个就绪标记文件握手，
#  所以先启动哪一边都可以（客户端最多等 45 秒）。
#
#  用法：
#    bash scripts/vsomeip_pair.sh server [用例]
#    bash scripts/vsomeip_pair.sh client [用例] [客户端参数...]
#
#  用例：
#    demo         车速服务示例（事件 + 同步/异步 RPC），默认
#    completion   异步 RPC 完成与超时边界
#    concurrency  多在途并发
#
#  产物：artifacts/vsomeip-pair/<用例>/
#    server.log   服务端原始输出
#    client.log   客户端原始输出
#    ready        服务端就绪标记
#    check.txt    客户端跑完后离线检查器的输出
#
#  自动编排（不需要人工开两个终端）请用：bash run-tests.sh vsomeip
# =============================================================================

set -uo pipefail

ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT" || exit 2

usage() {
    sed -n '2,/^# =\{20,\}$/p' "$ROOT/scripts/vsomeip_pair.sh" |
        sed -e '/^# =\+$/d' -e 's/^# \{0,1\}//'
}

ROLE="${1:-}"
CASE="${2:-demo}"
if [ "$#" -ge 2 ]; then shift 2; else shift "$#"; fi

case "$ROLE" in
    server|client) ;;
    -h|--help|help|"")
        usage
        exit 0
        ;;
    *)
        echo "未知角色：$ROLE（只支持 server 或 client）" >&2
        echo "" >&2
        usage >&2
        exit 2
        ;;
esac

# ---- 用例定义 ----
case "$CASE" in
    demo)
        SERVER_BIN="build/demo/demo_server"
        CLIENT_BIN="build/demo/demo_client"
        SERVER_ARGS="config/services.json"
        CLIENT_ARGS="config/services.json"
        CLIENT_TIMEOUT=8
        READY_PATTERN="Service running. Publishing speed events"
        CHECKER=""
        EXPECT_TIMEOUT=1
        ;;
    completion)
        SERVER_BIN="build-rpc-trace/rpc_completion_test"
        CLIENT_BIN="build-rpc-trace/rpc_completion_test"
        SERVER_ARGS="server"
        CLIENT_ARGS="client 100"
        CLIENT_TIMEOUT=180
        READY_PATTERN="SERVER_READY"
        CHECKER="scripts/check_rpc_completion_trace.py"
        EXPECT_TIMEOUT=0
        ;;
    concurrency)
        SERVER_BIN="build-rpc-trace/rpc_concurrency_test"
        CLIENT_BIN="build-rpc-trace/rpc_concurrency_test"
        SERVER_ARGS="server"
        CLIENT_ARGS="client 100 4"
        CLIENT_TIMEOUT=180
        READY_PATTERN="SERVER_READY"
        CHECKER="scripts/check_rpc_concurrency_trace.py"
        EXPECT_TIMEOUT=0
        ;;
    *)
        echo "未知用例：$CASE" >&2
        echo "" >&2
        usage >&2
        exit 2
        ;;
esac

if [ "$ROLE" = "client" ] && [ "$#" -gt 0 ]; then
    CLIENT_ARGS="$*"
fi

RUN_DIR="$ROOT/artifacts/vsomeip-pair/$CASE"
SERVER_LOG="$RUN_DIR/server.log"
CLIENT_LOG="$RUN_DIR/client.log"
READY_FILE="$RUN_DIR/ready"
CHECK_FILE="$RUN_DIR/check.txt"

# 服务端进程识别串：客户端用它判断"就绪标记"是不是仍然有效，
# 避免上一轮遗留的 ready 文件让客户端在没有服务端时空跑。
case "$SERVER_ARGS" in
    server) SERVER_PROC_PATTERN="$(basename -- "$SERVER_BIN") server" ;;
    *)      SERVER_PROC_PATTERN="$(basename -- "$SERVER_BIN")" ;;
esac
mkdir -p "$RUN_DIR"

hold_window() {
    [ -t 0 ] || return 0
    echo ""
    echo "（按回车关闭本窗口；5 分钟无操作也会自动关闭）"
    read -r -t 300 _ || true
}

banner() {
    echo "============================================================"
    echo "  ara-com vsomeip $1  用例=$CASE"
    echo "============================================================"
    echo "  二进制: $2"
    echo "  配置:   config/vsomeip.json"
    echo "  本侧日志: $3"
    echo "  对侧日志: $4"
    echo "------------------------------------------------------------"
}

# 服务端进程是否还在运行（用于判断就绪标记是否仍然有效）
server_alive() {
    pgrep -f "$SERVER_PROC_PATTERN" >/dev/null 2>&1
}
# ---- 服务端 ----
watch_ready() {
    local waited=0
    while [ "$waited" -lt 600 ]; do
        if [ -f "$SERVER_LOG" ] && grep -qF "$READY_PATTERN" "$SERVER_LOG" 2>/dev/null; then
            touch "$READY_FILE"
            echo ""
            echo "[就绪] 已检测到 \"$READY_PATTERN\"，客户端现在可以启动了。"
            return 0
        fi
        if [ "$waited" -eq 200 ]; then
            touch "$READY_FILE"
            echo ""
            echo "[提示] 20 秒内没等到就绪标记，已放行客户端；请确认服务端输出是否正常。"
            return 0
        fi
        sleep 0.1
        waited=$((waited + 1))
    done
}

run_server() {
    if [ ! -x "$SERVER_BIN" ]; then
        echo "错误：找不到可执行文件 $SERVER_BIN"
        echo "请先构建真实后端：bash run-tests.sh vsomeip"
        hold_window
        exit 2
    fi

    rm -f "$READY_FILE"
    banner "服务端" "$SERVER_BIN $SERVER_ARGS" "$SERVER_LOG" "$CLIENT_LOG"
    echo "  服务端同时是 routing manager，请保持本窗口运行，结束时按 Ctrl+C。"
    echo ""

    watch_ready &
    local watcher_pid=$!
    trap 'kill $watcher_pid 2>/dev/null || true' EXIT INT TERM

    export VSOMEIP_CONFIGURATION="$ROOT/config/vsomeip.json"
    export VSOMEIP_APPLICATION_NAME="ara_com_server_4660"

    if command -v stdbuf >/dev/null 2>&1; then
        stdbuf -oL -eL "$SERVER_BIN" $SERVER_ARGS 2>&1 | tee "$SERVER_LOG"
    else
        "$SERVER_BIN" $SERVER_ARGS 2>&1 | tee "$SERVER_LOG"
    fi

    echo ""
    echo "服务端已退出。原始日志：$SERVER_LOG"
    hold_window
}

# ---- 客户端 ----
run_client() {
    if [ ! -x "$CLIENT_BIN" ]; then
        echo "错误：找不到可执行文件 $CLIENT_BIN"
        echo "请先构建真实后端：bash run-tests.sh vsomeip"
        hold_window
        exit 2
    fi

    banner "客户端" "$CLIENT_BIN $CLIENT_ARGS" "$CLIENT_LOG" "$SERVER_LOG"
    echo "  客户端超时：${CLIENT_TIMEOUT}s"
    echo ""

    local waited=0
    while :; do
        if [ -f "$READY_FILE" ] && server_alive; then
            break
        fi
        if [ "$waited" -ge 450 ]; then
            echo ""
            echo "错误：等待服务端就绪超过 45 秒。"
            echo "  识别到的服务端进程：$SERVER_PROC_PATTERN"
            if server_alive; then
                echo "  服务端进程在运行，但没有落就绪标记，请检查服务端窗口的输出。"
            else
                echo "  没有检测到服务端进程，请在另一个终端先运行："
                echo "    bash scripts/vsomeip_pair.sh server $CASE"
            fi
            hold_window
            exit 2
        fi
        if [ "$waited" -eq 0 ]; then
            echo "等待服务端就绪（服务端进程：$SERVER_PROC_PATTERN）……"
        fi
        sleep 0.1
        waited=$((waited + 1))
        if [ $((waited % 50)) -eq 0 ]; then
            echo "  已等待 $((waited / 10)) 秒……"
        fi
    done

    echo "服务端已就绪，开始发送请求……"
    echo ""

    export VSOMEIP_CONFIGURATION="$ROOT/config/vsomeip.json"
    export VSOMEIP_APPLICATION_NAME="ara_com_client_4660"

    local rc=0
    if command -v stdbuf >/dev/null 2>&1; then
        stdbuf -oL -eL timeout --signal=TERM "${CLIENT_TIMEOUT}s" "$CLIENT_BIN" $CLIENT_ARGS 2>&1 | tee "$CLIENT_LOG"
    else
        timeout --signal=TERM "${CLIENT_TIMEOUT}s" "$CLIENT_BIN" $CLIENT_ARGS 2>&1 | tee "$CLIENT_LOG"
    fi
    rc=$?
    if [ "$EXPECT_TIMEOUT" -eq 1 ] && [ "$rc" -eq 124 ]; then
        rc=0
    fi

    echo ""
    echo "------------------------------------------------------------"
    local check_rc=0
    if [ -n "$CHECKER" ]; then
        echo "离线检查器：$CHECKER"
        python3 "$CHECKER" "$CLIENT_LOG" > "$CHECK_FILE" 2>&1
        check_rc=$?
        cat "$CHECK_FILE"
    else
        echo "关键日志检查："
        for pattern in "Service available" "Event received" "Sync RPC result" "Async RPC completed"; do
            if grep -qF "$pattern" "$CLIENT_LOG"; then
                echo "  [OK]   $pattern"
            else
                echo "  [缺失] $pattern"
                check_rc=1
            fi
        done
    fi
    echo "------------------------------------------------------------"
    echo "  客户端退出码: $rc（demo 用例被 timeout 正常终止时为 124，已按预期处理）"
    echo "  检查结果:     $check_rc"
    echo "  客户端日志:   $CLIENT_LOG"
    echo "  检查器输出:   $CHECK_FILE"

    if [ "$rc" -eq 0 ] && [ "$check_rc" -eq 0 ]; then
        echo "  结论: 通过"
        hold_window
        return 0
    fi
    echo "  结论: 失败"
    echo "  服务端窗口现在可以按 Ctrl+C 结束了。"
    hold_window
    return 1
}

if [ "$ROLE" = "server" ]; then
    run_server
else
    run_client
fi
