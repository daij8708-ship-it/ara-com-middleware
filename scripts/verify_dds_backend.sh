#!/usr/bin/env bash
# 用法：bash scripts/verify_dds_backend.sh [已构建目录]，不会安装依赖或执行构建。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${1:-$ROOT_DIR/build-dds}"
BIN="$BUILD_DIR/tools/dds_rpc_pair"
REQUESTS=200
SERVER_PID=""
CLIENT1_PID=""
CLIENT2_PID=""
LOCK_DIR=""
OUT_DIR=""

if [[ ! -x "$BIN" && -x "$BIN.exe" ]]; then
    BIN="$BIN.exe"
fi
if [[ ! -x "$BIN" ]]; then
    printf '未找到已构建的 DDS 工具：%s\n' "$BIN" >&2
    exit 2
fi
for program in timeout grep date mkdir rmdir; do
    command -v "$program" >/dev/null || { printf '缺少命令：%s\n' "$program" >&2; exit 2; }
done

cleanup() {
    local pid
    trap - EXIT INT TERM
    # 仅向本脚本保存的自起进程发信号，禁止按进程名或进程组清理。
    for pid in "$CLIENT1_PID" "$CLIENT2_PID" "$SERVER_PID"; do
        if [[ -n "$pid" ]]; then kill -TERM "$pid" 2>/dev/null || true; fi
    done
    for pid in "$CLIENT1_PID" "$CLIENT2_PID" "$SERVER_PID"; do
        if [[ -n "$pid" ]]; then wait "$pid" 2>/dev/null || true; fi
    done
    if [[ -n "$LOCK_DIR" ]]; then rmdir "$LOCK_DIR" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

fail() {
    printf 'FAIL: %s\n日志目录：%s\n' "$*" "$OUT_DIR" >&2
    exit 1
}

mkdir -p "$ROOT_DIR/artifacts"
STAMP="$(date +%Y%m%d-%H%M%S)-$$-$RANDOM"
OUT_DIR="$ROOT_DIR/artifacts/dds-backend-$STAMP"
mkdir "$OUT_DIR"

# 原子占用独立测试域，避开单元测试域 61，随机服务编号进一步隔离其他进程。
SEED=$((RANDOM + $$))
for ((attempt = 0; attempt < 119; ++attempt)); do
    DOMAIN=$((62 + (SEED + attempt) % 119))
    candidate="$ROOT_DIR/artifacts/.dds-domain-$DOMAIN.lock"
    if mkdir "$candidate" 2>/dev/null; then
        LOCK_DIR="$candidate"
        break
    fi
done
[[ -n "$LOCK_DIR" ]] || fail '没有空闲的独立 DDS 测试域'
SID=$((32768 + (RANDOM * 32768 + RANDOM + $$) % 32767))
printf 'sid=%s\ndomain=%s\nrequests_per_client=%s\nbinary=%s\n' \
    "$SID" "$DOMAIN" "$REQUESTS" "$BIN" > "$OUT_DIR/run.txt"

# 超时包装器只管理其自起工具，超时后最多再留五秒执行正常停止。
timeout --signal=TERM --kill-after=5s 90s \
    "$BIN" server "$SID" "$DOMAIN" 85000 > "$OUT_DIR/server.log" 2>&1 &
SERVER_PID=$!
ready=0
for ((attempt = 0; attempt < 300; ++attempt)); do
    if grep -Fxq "SERVER_READY sid=$SID domain=$DOMAIN" "$OUT_DIR/server.log"; then
        ready=1
        break
    fi
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.05
done
[[ "$ready" -eq 1 ]] || fail '服务端未发出准确的就绪信号'

# 两个独立客户端并行，各自交替提交异步和同步请求并校验完整负载。
timeout --signal=TERM --kill-after=5s 60s \
    "$BIN" client "$SID" "$DOMAIN" 1 "$REQUESTS" > "$OUT_DIR/client1.log" 2>&1 &
CLIENT1_PID=$!
timeout --signal=TERM --kill-after=5s 60s \
    "$BIN" client "$SID" "$DOMAIN" 2 "$REQUESTS" > "$OUT_DIR/client2.log" 2>&1 &
CLIENT2_PID=$!

RC1=0
RC2=0
wait "$CLIENT1_PID" || RC1=$?
CLIENT1_PID=""
wait "$CLIENT2_PID" || RC2=$?
CLIENT2_PID=""
printf 'client1=%s\nclient2=%s\n' "$RC1" "$RC2" > "$OUT_DIR/exit-codes.txt"
[[ "$RC1" -eq 0 ]] || fail "客户端 1 退出码 $RC1"
[[ "$RC2" -eq 0 ]] || fail "客户端 2 退出码 $RC2"
kill -0 "$SERVER_PID" 2>/dev/null || fail '客户端结束前服务端已经退出'

check_result() {
    local id="$1" log="$2" count pattern
    count="$(grep -c '^CLIENT_RESULT' "$log" || true)"
    [[ "$count" == 1 ]] || fail "客户端 $id 必须且只能输出一条结果"
    pattern="CLIENT_RESULT id=$id requests=$REQUESTS sync=$((REQUESTS / 2)) async=$(((REQUESTS + 1) / 2)) ok=$REQUESTS fail=0 callbacks=$(((REQUESTS + 1) / 2)) duplicates=0 events=[1-9][0-9]* bad_events=0 available=1"
    grep -Ex "$pattern" "$log" || fail "客户端 $id 的完整结果不匹配"
}
RESULT1="$(check_result 1 "$OUT_DIR/client1.log")" || fail '客户端 1 核对失败'
RESULT2="$(check_result 2 "$OUT_DIR/client2.log")" || fail '客户端 2 核对失败'

kill -TERM "$SERVER_PID" 2>/dev/null || true
SERVER_RC=0
wait "$SERVER_PID" || SERVER_RC=$?
SERVER_PID=""
printf 'server=%s\n' "$SERVER_RC" >> "$OUT_DIR/exit-codes.txt"
[[ "$SERVER_RC" -eq 0 || "$SERVER_RC" -eq 143 ]] || fail "服务端停止退出码 $SERVER_RC"
grep -Exq "SERVER_DONE requests=$((REQUESTS * 2)) events=[1-9][0-9]*" "$OUT_DIR/server.log" \
    || fail '服务端未正常停止或实际处理请求数不准确'

wait_marker() {
    local marker="$1" log="$2" pid="$3"
    for ((attempt = 0; attempt < 300; ++attempt)); do
        if grep -Fxq "$marker" "$log"; then return 0; fi
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.05
    done
    return 1
}

# 此阶段保存真实服务端 PID，只对本次创建的进程模拟崩溃。
"$BIN" server "$SID" "$DOMAIN" 40000 > "$OUT_DIR/crash-server.log" 2>&1 &
SERVER_PID=$!
wait_marker "SERVER_READY sid=$SID domain=$DOMAIN" "$OUT_DIR/crash-server.log" "$SERVER_PID" \
    || fail '崩溃测试服务端未就绪'
timeout --signal=TERM --kill-after=5s 50s \
    "$BIN" recover "$SID" "$DOMAIN" 3 > "$OUT_DIR/recovery-client.log" 2>&1 &
CLIENT1_PID=$!
wait_marker 'CLIENT_RECOVERY_READY id=3' "$OUT_DIR/recovery-client.log" "$CLIENT1_PID" \
    || fail '恢复客户端尚未完成初始 RPC 和事件订阅'
kill -KILL "$SERVER_PID"
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""
wait_marker 'CLIENT_RECOVERY_LOST id=3' "$OUT_DIR/recovery-client.log" "$CLIENT1_PID" \
    || fail '租约到期后未报告服务不可用'
"$BIN" server "$SID" "$DOMAIN" 30000 > "$OUT_DIR/restarted-server.log" 2>&1 &
SERVER_PID=$!
RECOVERY_RC=0
wait "$CLIENT1_PID" || RECOVERY_RC=$?
CLIENT1_PID=""
printf 'recovery=%s\n' "$RECOVERY_RC" >> "$OUT_DIR/exit-codes.txt"
[[ "$RECOVERY_RC" -eq 0 ]] || fail '客户端未恢复 RPC 和事件'
RECOVERY='RECOVERY_RESULT id=3 unavailable=1 recovered=1 rpc=1 events=1'
[[ "$(grep -c '^RECOVERY_RESULT' "$OUT_DIR/recovery-client.log" || true)" == 1 ]] \
    || fail '恢复结果缺失或重复'
grep -Fxq "$RECOVERY" "$OUT_DIR/recovery-client.log" || fail '恢复结果不匹配'
kill -TERM "$SERVER_PID"
wait "$SERVER_PID" || fail '重启后的服务端未正常退出'
SERVER_PID=""

printf '%s\n%s\n%s\n' "$RESULT1" "$RESULT2" "$RECOVERY" > "$OUT_DIR/results.log"
printf '%s\n%s\n%s\n' "$RESULT1" "$RESULT2" "$RECOVERY"
printf '证据目录：%s\n' "$OUT_DIR" >&2
