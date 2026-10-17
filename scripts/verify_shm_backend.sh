#!/usr/bin/env bash
# 共享内存 RPC 后端双进程验证：一个服务端 + 两个客户端。
#
# 用法：bash scripts/verify_shm_backend.sh [构建目录]

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${1:-$ROOT_DIR/build-shm}"
BIN="$BUILD_DIR/tools/shm_rpc_pair"
REQUESTS=200

if [ ! -x "$BIN" ]; then
    echo "未找到 $BIN，请先构建 shm 目标（-DARA_COM_BUILD_SHM=ON）。" >&2
    exit 2
fi

STAMP="$(date +%Y%m%d-%H%M%S)"
OUT_DIR="$ROOT_DIR/artifacts/shm-backend-$STAMP"
mkdir -p "$OUT_DIR"

SID=$((0x7A00 + ($$ % 200)))
SERVER_PID=""
CLIENT_PIDS=()

cleanup() {
    for pid in "${CLIENT_PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

fail() {
    echo "FAIL: $*" >&2
    echo "日志目录：$OUT_DIR" >&2
    exit 1
}

"$BIN" server "$SID" 20000 > "$OUT_DIR/server.log" 2>&1 &
SERVER_PID=$!

ready=0
for _ in $(seq 1 100); do
    if grep -q "SERVER_READY" "$OUT_DIR/server.log"; then
        ready=1
        break
    fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        break
    fi
    sleep 0.05
done
[ "$ready" -eq 1 ] || fail "服务端未就绪"

timeout 20s "$BIN" client "$SID" 1 "$REQUESTS" > "$OUT_DIR/client1.log" 2>&1 &
CLIENT_PIDS+=($!)
timeout 20s "$BIN" client "$SID" 2 "$REQUESTS" > "$OUT_DIR/client2.log" 2>&1 &
CLIENT_PIDS+=($!)

RC1=0
RC2=0
wait "${CLIENT_PIDS[0]}" || RC1=$?
wait "${CLIENT_PIDS[1]}" || RC2=$?
CLIENT_PIDS=()

[ "$RC1" -eq 0 ] || fail "客户端 1 退出码 $RC1"
[ "$RC2" -eq 0 ] || fail "客户端 2 退出码 $RC2"

grep -q "CLIENT_RESULT id=1 ok=$REQUESTS fail=0 events=[1-9]" "$OUT_DIR/client1.log" \
    || fail "客户端 1 结果不符合预期"
grep -q "CLIENT_RESULT id=2 ok=$REQUESTS fail=0 events=[1-9]" "$OUT_DIR/client2.log" \
    || fail "客户端 2 结果不符合预期"

kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

echo "BACKEND_SUMMARY clients=2 requests=$REQUESTS ok=1"
echo "证据目录：$OUT_DIR"
