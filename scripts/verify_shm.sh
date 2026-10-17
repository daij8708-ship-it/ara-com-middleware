#!/usr/bin/env bash
# =============================================================================
#  同机共享内存通道双进程验证
#
#  覆盖：小消息吞吐/延迟、64 KB 大消息、客户端先启动、服务端崩溃与陈旧
#  共享内存清理。每个用例使用独立通道名，产物保存在 artifacts/ 下。
#
#  用法：bash scripts/verify_shm.sh [构建目录]
#  默认构建目录：<项目根>/build-shm
# =============================================================================

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-$root/build-shm}"
bin="$build_dir/tools/shm_pair"
if [[ ! -x "$bin" ]]; then
    bin="$build_dir/shm_pair"
fi
if [[ ! -x "$bin" ]]; then
    echo "找不到 shm_pair：$build_dir/tools/shm_pair" >&2
    echo "请先配置 -DARA_COM_BUILD_SHM=ON 并编译。" >&2
    exit 2
fi

run_stamp="$(date +%Y%m%d-%H%M%S)"
artifact_dir="$root/artifacts/shm-validation-$run_stamp"
mkdir -p "$artifact_dir"

server_pid=""
client_pid=""

cleanup() {
    for pid in "$server_pid" "$client_pid"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
        fi
    done
}
trap cleanup EXIT INT TERM

fail() {
    echo "失败：$*" >&2
    echo "证据目录：$artifact_dir" >&2
    exit 1
}

check_result() {
    local rc="$1"
    local logfile="$2"
    local expected="$3"
    local label="$4"

    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$logfile" >&2 || true
        fail "$label 客户端退出码 $rc"
    fi

    local line=""
    if ! line="$(grep -E '^RESULT ' "$logfile" | tail -n 1)"; then
        tail -n 20 "$logfile" >&2 || true
        fail "$label 没有 RESULT 行"
    fi
    echo "  $line"

    local ok=""
    local total=""
    ok="$(sed -n 's/.*ok=\([0-9][0-9]*\).*/\1/p' <<<"$line")"
    total="$(sed -n 's/.*total=\([0-9][0-9]*\).*/\1/p' <<<"$line")"
    [[ -n "$ok" && -n "$total" ]] || fail "$label RESULT 字段缺失"
    [[ "$ok" == "$expected" && "$total" == "$expected" ]] ||
        fail "$label 期望 $expected 条，实际 total=$total ok=$ok"
}

wait_server() {
    if [[ -n "$server_pid" ]]; then
        wait "$server_pid" 2>/dev/null || true
        server_pid=""
    fi
}

echo "构建目录：$build_dir"
echo "证据目录：$artifact_dir"

# ---- 用例 1：小消息吞吐与延迟 ----
echo ""
echo "▶ 用例 1：100000 条 64 B 消息"
name="shmtest_$(date +%s)_$$_small"
"$bin" server "$name" 100000 >"$artifact_dir/small-server.log" 2>&1 &
server_pid=$!
sleep 0.2
set +e
timeout 120s "$bin" client "$name" 100000 64 64 >"$artifact_dir/small-client.log" 2>&1
rc=$?
set -e
wait_server
check_result "$rc" "$artifact_dir/small-client.log" 100000 "小消息"
echo "✔ 用例 1 通过"

# ---- 用例 2：64 KB 大消息（65528 B 载荷 + 8 B 测试头）----
echo ""
echo "▶ 用例 2：2000 条 64 KB 消息"
name="shmtest_$(date +%s)_$$_large"
"$bin" server "$name" 2000 >"$artifact_dir/large-server.log" 2>&1 &
server_pid=$!
sleep 0.2
set +e
timeout 120s "$bin" client "$name" 2000 65528 16 >"$artifact_dir/large-client.log" 2>&1
rc=$?
set -e
wait_server
check_result "$rc" "$artifact_dir/large-client.log" 2000 "大消息"
echo "✔ 用例 2 通过"

# ---- 用例 3：客户端先启动 ----
echo ""
echo "▶ 用例 3：客户端先启动，服务端后启动"
name="shmtest_$(date +%s)_$$_clientfirst"
set +e
timeout 120s "$bin" client "$name" 20000 64 64 >"$artifact_dir/clientfirst-client.log" 2>&1 &
client_pid=$!
sleep 0.3
"$bin" server "$name" 20000 >"$artifact_dir/clientfirst-server.log" 2>&1 &
server_pid=$!
wait "$client_pid"
rc=$?
set -e
client_pid=""
wait_server
check_result "$rc" "$artifact_dir/clientfirst-client.log" 20000 "客户端先启动"
echo "✔ 用例 3 通过"

# ---- 用例 4：服务端崩溃 + 陈旧对象清理 ----
echo ""
echo "▶ 用例 4：服务端被 kill -9 后客户端退出，重启后清理陈旧对象"
name="shmtest_$(date +%s)_$$_crash"
"$bin" server "$name" 0 >"$artifact_dir/crash-server.log" 2>&1 &
server_pid=$!
sleep 0.2
set +e
timeout 60s "$bin" client "$name" 2000000 65528 16 >"$artifact_dir/crash-client.log" 2>&1 &
client_pid=$!
sleep 0.5
kill -9 "$server_pid" 2>/dev/null || true
wait "$server_pid" 2>/dev/null || true
server_pid=""
wait "$client_pid"
crash_rc=$?
set -e
client_pid=""

if [[ "$crash_rc" -eq 0 ]]; then
    fail "服务端被 kill 后客户端仍正常结束（应检测到对端消失）"
fi
if [[ "$crash_rc" -eq 124 ]]; then
    fail "服务端被 kill 后客户端超时挂住"
fi
echo "  客户端退出码 $crash_rc（预期非 0，且不是 124）"

# 同一名字重新启动：init_server 应识别 creator_pid 已死并清理陈旧 shm/sem。
"$bin" server "$name" 1000 >"$artifact_dir/restart-server.log" 2>&1 &
server_pid=$!
sleep 0.2
set +e
timeout 120s "$bin" client "$name" 1000 64 64 >"$artifact_dir/restart-client.log" 2>&1
restart_rc=$?
set -e
wait_server
check_result "$restart_rc" "$artifact_dir/restart-client.log" 1000 "重启后"

if ls /dev/shm 2>/dev/null | grep -F "ara_com_${name}_" >/dev/null; then
    fail "重启并退出后仍有残留的共享内存/信号量对象"
fi
echo "✔ 用例 4 通过"

# ---- 环境信息 ----
{
    echo "commit=$(git -C "$root" rev-parse HEAD 2>/dev/null || echo uncommitted)"
    echo "date=$(date --iso-8601=seconds)"
    echo "kernel=$(uname -srmo)"
    echo "compiler=$(${CXX:-c++} --version | head -n 1)"
    echo "cmake=$(cmake --version | head -n 1)"
    echo "build_dir=$build_dir"
} >"$artifact_dir/environment.txt"

echo ""
echo "共享内存通道双进程验证通过。"
echo "证据目录：$artifact_dir"
