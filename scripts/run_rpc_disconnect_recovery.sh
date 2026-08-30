#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bin="$root/build-rpc-disconnect/rpc_disconnect_recovery_test"
run_dir="$root/tests/reports/staging/rpc-disconnect-$(date +%Y%m%d-%H%M%S-%N)"
mkdir -p "$run_dir"
printf '%s\n' "$run_dir" > /tmp/ara_rpc_disconnect_dir
export VSOMEIP_CONFIGURATION="$root/config/vsomeip-disconnect-test.json"
router_pid=""
server_pid=""
client_pid=""

cleanup() {
    [[ -z "$router_pid" ]] || ! kill -0 "$router_pid" 2>/dev/null || kill -TERM "$router_pid"
    [[ -z "$client_pid" ]] || ! kill -0 "$client_pid" 2>/dev/null || kill -TERM "$client_pid"
    [[ -z "$server_pid" ]] || ! kill -0 "$server_pid" 2>/dev/null || kill -TERM "$server_pid"
}
trap cleanup EXIT INT TERM

start_server() {
    VSOMEIP_APPLICATION_NAME=ara_com_server_4660 "$bin" server >"$1" 2>&1 &
    server_pid=$!
}

VSOMEIP_APPLICATION_NAME=ara_com_router_4660 "$bin" router >"$run_dir/router.stdout.log" 2>&1 &
router_pid=$!
sleep 1
kill -0 "$router_pid"
start_server "$run_dir/server-before.stdout.log"
sleep 1
kill -0 "$server_pid"
VSOMEIP_APPLICATION_NAME=ara_com_client_4660 timeout 90s "$bin" client "$run_dir" \
    >"$run_dir/client.stdout.log" 2>&1 &
client_pid=$!

for _ in $(seq 1 200); do
    [[ -f "$run_dir/ready-to-kill" ]] && break
    sleep .05
done
[[ -f "$run_dir/ready-to-kill" ]] || {
    echo 'orchestrator: no kill marker' >&2
    exit 2
}
kill -TERM "$server_pid"
wait "$server_pid" 2>/dev/null || true
server_pid=""

for _ in $(seq 1 300); do
    [[ -f "$run_dir/ready-to-restart" ]] && break
    sleep .05
done
[[ -f "$run_dir/ready-to-restart" ]] || {
    echo 'orchestrator: no restart marker' >&2
    exit 2
}
start_server "$run_dir/server-after.stdout.log"
set +e
wait "$client_pid"
test_exit=$?
set -e
client_pid=""
printf 'test_exit=%s\n' "$test_exit" | tee "$run_dir/test-exit.txt"
set +e
python3 "$root/scripts/check_rpc_disconnect_recovery.py" "$run_dir/client.stdout.log" | tee "$run_dir/check.txt"
# 先保存整条管道的状态，下一条命令会覆盖 PIPESTATUS。
check_status=("${PIPESTATUS[@]}")
set -e
check_exit=${check_status[0]}
printf 'check_exit=%s\ntee_exit=%s\n' "$check_exit" "${check_status[1]}" | tee "$run_dir/check-exit.txt"
printf 'run_dir=%s\n' "$run_dir"
[[ $test_exit -eq 0 && $check_exit -eq 0 && ${check_status[1]} -eq 0 ]]
