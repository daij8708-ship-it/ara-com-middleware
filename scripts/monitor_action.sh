#!/usr/bin/env bash
# 面板后台动作：按需构建并启动 DDS / SOME/IP 演示，或运行测试。
#
# 由 tools/monitor_server.py 调用，也可以手动执行：
#   ARA_COM_MONITOR_RUN_DIR=artifacts/manual bash scripts/monitor_action.sh demo dds
#   ARA_COM_MONITOR_RUN_DIR=artifacts/manual bash scripts/monitor_action.sh demo vsomeip
#   ARA_COM_MONITOR_RUN_DIR=artifacts/manual bash scripts/monitor_action.sh tests quick
#
# 环境变量：
#   ARA_COM_MONITOR_RUN_DIR       必填，指标、日志和动作输出目录
#   ARA_COM_MONITOR_BUILD_DIR     构建目录，默认 <项目根>/build-monitor
#   ARA_COM_MONITOR_DURATION      演示时长（秒），默认 120
#   ARA_COM_MONITOR_REQUESTS      DDS 每个客户端的 RPC 次数，默认 300
#   ARA_COM_MONITOR_CLIENTS       DDS 每批客户端数，默认 4
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-}"
TARGET="${2:-}"
RUN_DIR="${ARA_COM_MONITOR_RUN_DIR:-}"
BUILD_DIR="${ARA_COM_MONITOR_BUILD_DIR:-$ROOT/build-monitor}"
DURATION="${ARA_COM_MONITOR_DURATION:-120}"
REQUESTS="${ARA_COM_MONITOR_REQUESTS:-300}"
CLIENTS="${ARA_COM_MONITOR_CLIENTS:-4}"
CHILDREN=()

usage() {
    sed -n '2,17p' "$ROOT/scripts/monitor_action.sh" | sed -e 's/^# \{0,1\}//'
}

if [ -z "$RUN_DIR" ]; then
    echo "缺少 ARA_COM_MONITOR_RUN_DIR。" >&2
    exit 2
fi

case "$DURATION" in
    ''|*[!0-9]*) echo "演示时长必须是整数秒。" >&2; exit 2 ;;
esac
if [ "$DURATION" -lt 5 ] || [ "$DURATION" -gt 3600 ]; then
    echo "演示时长必须在 5 到 3600 秒之间。" >&2
    exit 2
fi
case "$REQUESTS" in
    ''|*[!0-9]*) echo "RPC 次数必须是整数。" >&2; exit 2 ;;
esac
case "$CLIENTS" in
    ''|*[!0-9]*) echo "客户端数必须是整数。" >&2; exit 2 ;;
esac
if [ "$REQUESTS" -lt 1 ] || [ "$REQUESTS" -gt 10000 ] || [ "$CLIENTS" -lt 1 ] || [ "$CLIENTS" -gt 8 ]; then
    echo "RPC 次数或客户端数超出范围。" >&2
    exit 2
fi

mkdir -p "$RUN_DIR" "$RUN_DIR/metrics" "$RUN_DIR/logs" "$RUN_DIR/actions"

cleanup() {
    set +e
    for pid in "${CHILDREN[@]:-}"; do
        kill -TERM "$pid" 2>/dev/null
    done
    for pid in "${CHILDREN[@]:-}"; do
        wait "$pid" 2>/dev/null
    done
}
trap cleanup EXIT INT TERM HUP

# 监测环境变量对两种后端完全一致，面板据此过滤。
export ARA_COM_LOG_DIR="$RUN_DIR/logs"
export ARA_COM_METRICS_DIR="$RUN_DIR/metrics"
export ARA_COM_METRICS_INTERVAL_MS=200
export ARA_COM_LOG_ROTATE_BYTES=2097152
export ARA_COM_LOG_MAX_FILES=5

configure_build() {
    cmake -S "$ROOT" -B "$BUILD_DIR" \
        -DARA_COM_USE_DDS=ON \
        -DARA_COM_USE_VSOMEIP=ON \
        -DARA_COM_BUILD_TESTS=OFF \
        -DARA_COM_BUILD_DEMOS=ON \
        -DARA_COM_BUILD_BENCHMARKS=OFF \
        -DCMAKE_BUILD_TYPE=Release
}

wait_for_text() {
    local file="$1"
    local pattern="$2"
    local timeout_s="$3"
    local waited=0
    while [ "$waited" -lt $((timeout_s * 10)) ]; do
        if [ -f "$file" ] && grep -qF "$pattern" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
        waited=$((waited + 1))
    done
    return 1
}

run_dds_demo() {
    configure_build
    cmake --build "$BUILD_DIR" --target dds_rpc_pair -j"$(nproc)"

    local bin="$BUILD_DIR/tools/dds_rpc_pair"
    local sid=$(( (RANDOM % 50000) + 10000 ))
    local domain=$(( RANDOM % 200 ))
    local server_out="$RUN_DIR/dds-server.out"

    "$bin" server "$sid" "$domain" $((DURATION * 1000)) >"$server_out" 2>&1 &
    local server_pid=$!
    CHILDREN+=("$server_pid")

    if ! wait_for_text "$server_out" "SERVER_READY" 45; then
        echo "DDS 服务端启动失败：" >&2
        cat "$server_out" >&2 || true
        exit 1
    fi
    echo "DDS 服务端就绪，服务号 $sid，域 $domain。"
    echo "每个客户端发送 $REQUESTS 次调用，共两批、每批 $CLIENTS 个客户端。"

    run_wave() {
        local base="$1"
        local pids=()
        local offset client_id pid
        for offset in $(seq 1 "$CLIENTS"); do
            client_id=$((base + offset))
            "$bin" client "$sid" "$domain" "$client_id" "$REQUESTS" \
                >"$RUN_DIR/dds-client-$client_id.out" 2>&1 &
            pid=$!
            pids+=("$pid")
            CHILDREN+=("$pid")
        done
        for pid in "${pids[@]}"; do
            if ! wait "$pid"; then
                echo "客户端 $pid 失败，详见 dds-client-*.out。" >&2
            fi
        done
    }

    echo "第一批 DDS 客户端..."
    run_wave 10
    echo "第二批 DDS 客户端..."
    run_wave 20

    echo "DDS 流量采集完成，等待服务端运行到设定时长..."
    wait "$server_pid"
    echo "DDS 演示结束。"
}

run_vsomeip_demo() {
    configure_build
    cmake --build "$BUILD_DIR" --target demo_server demo_client -j"$(nproc)"

    local server_bin="$BUILD_DIR/demo/demo_server"
    local client_bin="$BUILD_DIR/demo/demo_client"
    local server_out="$RUN_DIR/vsomeip-server.out"
    local client_out="$RUN_DIR/vsomeip-client.out"

    # 与 scripts/vsomeip_pair.sh 使用同一份配置和应用名。
    VSOMEIP_CONFIGURATION="$ROOT/config/vsomeip.json" \
    VSOMEIP_APPLICATION_NAME="ara_com_server_4660" \
    ARA_COM_DEMO_ROLE="vsomeip-server" \
        "$server_bin" "$ROOT/config/services.json" >"$server_out" 2>&1 &
    local server_pid=$!
    CHILDREN+=("$server_pid")

    if ! wait_for_text "$server_out" "Service running. Publishing speed events" 45; then
        echo "SOME/IP 服务端启动失败：" >&2
        cat "$server_out" >&2 || true
        exit 1
    fi
    echo "SOME/IP 服务端就绪，启动客户端..."

    VSOMEIP_CONFIGURATION="$ROOT/config/vsomeip.json" \
    VSOMEIP_APPLICATION_NAME="ara_com_client_4660" \
    ARA_COM_DEMO_ROLE="vsomeip-client-1" \
        "$client_bin" "$ROOT/config/services.json" >"$client_out" 2>&1 &
    local client_pid=$!
    CHILDREN+=("$client_pid")

    echo "SOME/IP 演示运行 ${DURATION} 秒，按停止按钮可提前结束。"
    local deadline=$((SECONDS + DURATION))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if ! kill -0 "$server_pid" 2>/dev/null; then
            echo "SOME/IP 服务端已退出。" >&2
            break
        fi
        if ! kill -0 "$client_pid" 2>/dev/null; then
            echo "SOME/IP 客户端已退出。" >&2
            break
        fi
        sleep 1
    done
    echo "SOME/IP 演示结束。"
}

run_tests() {
    local suite="$1"
    local args=()
    case "$suite" in
        quick) args=(quick) ;;
        functional) args=() ;;
        unit|guards|shm|vsomeip|recovery|concurrency|dds|asan|tsan|capacity|all)
            args=("$suite") ;;
        *) echo "未知测试档位：$suite" >&2; exit 2 ;;
    esac

    bash "$ROOT/run-tests.sh" "${args[@]}" &
    CHILDREN+=("$!")
    wait "$!"
}

case "$MODE" in
    demo)
        case "$TARGET" in
            dds) run_dds_demo ;;
            vsomeip) run_vsomeip_demo ;;
            *) echo "未知后端：$TARGET" >&2; exit 2 ;;
        esac
        ;;
    tests)
        run_tests "$TARGET"
        ;;
    -h|--help|help|"")
        usage
        exit 0
        ;;
    *)
        echo "未知模式：$MODE" >&2
        usage >&2
        exit 2
        ;;
esac
