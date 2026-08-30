#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bin="$root/build-vsomeip-capacity/vsomeip_capacity"
run="$root/tests/reports/staging/vsomeip-capacity-$(date +%Y%m%d-%H%M%S-%N)"
mkdir -p "$run"
echo "$run" > /tmp/ara_capacity_dir
export VSOMEIP_CONFIGURATION="$root/config/vsomeip-disconnect-test.json"
router=""
server=""

cleanup() {
    [[ -z "$server" ]] || ! kill -0 "$server" 2>/dev/null || kill -TERM "$server"
    [[ -z "$router" ]] || ! kill -0 "$router" 2>/dev/null || kill -TERM "$router"
}
trap cleanup EXIT INT TERM

VSOMEIP_APPLICATION_NAME=ara_com_router_4660 "$bin" router >"$run/router.log" 2>&1 &
router=$!
sleep 1
kill -0 "$router"
VSOMEIP_APPLICATION_NAME=ara_com_server_4660 "$bin" server >"$run/server.log" 2>&1 &
server=$!
sleep 1
kill -0 "$server"

: >"$run/results.txt"
for w in 1 4 16 32 64 128; do
  for rep in 1 2 3; do
    echo "PROGRESS window=$w repeat=$rep" | tee -a "$run/progress.txt"
    set +e
    /usr/bin/time -v -o "$run/client-w${w}-r${rep}.time.txt" \
      env VSOMEIP_APPLICATION_NAME=ara_com_client_4660 \
      timeout 90s "$bin" client "$w" 10 60 >"$run/client-w${w}-r${rep}.log" 2>&1
    test_exit=$?
    set -e
    printf 'test_exit=%s\n' "$test_exit" >"$run/client-w${w}-r${rep}.exit.txt"
    if [[ $test_exit -ne 0 ]]; then
      printf 'test_exit=%s\n' "$test_exit" >"$run/test-exit.txt"
      printf 'check_exit=not_run\n' >"$run/check-exit.txt"
      printf 'run_dir=%s\n' "$run"
      exit "$test_exit"
    fi
    grep '^RESULT ' "$run/client-w${w}-r${rep}.log" \
      | sed "s/^RESULT /RESULT repeat=$rep /" \
      | tee -a "$run/results.txt"
    ps -p "$server" -o pid=,%cpu=,rss=,nlwp= >"$run/server-w${w}-r${rep}.ps.txt"
    sleep 1
  done
done
printf 'test_exit=0\n' >"$run/test-exit.txt"
set +e
python3 "$root/scripts/check_vsomeip_capacity.py" "$run/results.txt" | tee "$run/check.txt"
# 检查失败也先记下退出码，再决定脚本是否失败。
check_status=("${PIPESTATUS[@]}")
set -e
rc=${check_status[0]}
printf 'check_exit=%s\ntee_exit=%s\n' "$rc" "${check_status[1]}" >"$run/check-exit.txt"
printf 'run_dir=%s\n' "$run"
if [[ $rc -ne 0 ]]; then
  exit "$rc"
fi
exit "${check_status[1]}"
