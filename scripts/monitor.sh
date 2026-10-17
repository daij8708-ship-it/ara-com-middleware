#!/usr/bin/env bash
# ara-com 一键运行监测：启动本机面板，按需构建并运行 DDS / SOME/IP 演示。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${ARA_COM_MONITOR_PORT:-8099}"
DURATION="${ARA_COM_MONITOR_DURATION:-120}"
REQUESTS="${ARA_COM_MONITOR_REQUESTS:-300}"
CLIENTS="${ARA_COM_MONITOR_CLIENTS:-4}"
BUILD_DIR="${ARA_COM_MONITOR_BUILD_DIR:-$ROOT/build-monitor}"
OPEN_BROWSER=1
ATTACH_DIR=""
AUTOSTART="dds"
AUTOSTART_SET=0
CHILDREN=()

usage() {
    cat <<'EOF'
用法: bash scripts/monitor.sh [选项]

  --attach <目录>     只打开面板，读取已有运行目录（目录下需有 metrics/ 和 logs/）
  --autostart <后端>  面板启动后自动运行的演示：dds、vsomeip 或 none（默认 dds）
  --port <端口>       面板端口，默认 8099
  --duration <秒>     演示运行时长，默认 120
  --requests <次数>   DDS 每个客户端发起的 RPC 数，默认 300
  --clients <数量>    DDS 每批客户端数，默认 4
  --no-open           不自动打开浏览器
  -h, --help          显示本帮助

面板内可以切换 DDS / SOME/IP 视图，启动或停止演示，并运行快速回归、功能回归或全部测试。
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --attach)    ATTACH_DIR="${2:-}"; shift 2 ;;
        --autostart) AUTOSTART="${2:-}"; AUTOSTART_SET=1; shift 2 ;;
        --port)      PORT="${2:-}"; shift 2 ;;
        --duration)  DURATION="${2:-}"; shift 2 ;;
        --requests)  REQUESTS="${2:-}"; shift 2 ;;
        --clients)   CLIENTS="${2:-}"; shift 2 ;;
        --no-open)   OPEN_BROWSER=0; shift ;;
        -h|--help)   usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; usage; exit 2 ;;
    esac
done

case "$AUTOSTART" in
    dds|vsomeip|none) ;;
    *) echo "未知自动启动后端: $AUTOSTART（可选 dds、vsomeip、none）" >&2; exit 2 ;;
esac

if ! command -v python3 >/dev/null 2>&1; then
    echo "缺少 python3，无法启动面板。" >&2
    exit 2
fi

if [ -n "$ATTACH_DIR" ]; then
    if [ ! -d "$ATTACH_DIR" ]; then
        echo "运行目录不存在: $ATTACH_DIR" >&2
        exit 2
    fi
    RUN_DIR="$(cd "$ATTACH_DIR" && pwd)"
    if [ "$AUTOSTART_SET" = "0" ]; then
        AUTOSTART="none"
    fi
else
    RUN_DIR="$ROOT/artifacts/monitor-$(date +%Y%m%d-%H%M%S)-$$"
fi
mkdir -p "$RUN_DIR/metrics" "$RUN_DIR/logs" "$RUN_DIR/actions"

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

python3 "$ROOT/tools/monitor_server.py" \
    --root "$ROOT" \
    --run-dir "$RUN_DIR" \
    --build-dir "$BUILD_DIR" \
    --port "$PORT" \
    --autostart "$AUTOSTART" \
    --duration "$DURATION" \
    --requests "$REQUESTS" \
    --clients "$CLIENTS" \
    >"$RUN_DIR/monitor.log" 2>&1 &
WEB_PID=$!
CHILDREN+=("$WEB_PID")

URL="http://127.0.0.1:$PORT/"
READY=0
for _ in $(seq 1 100); do
    if python3 - "$URL/api/health" <<'PY' >/dev/null 2>&1
import sys, urllib.request
urllib.request.urlopen(sys.argv[1], timeout=0.5).read()
PY
    then
        READY=1
        break
    fi
    if ! kill -0 "$WEB_PID" 2>/dev/null; then
        break
    fi
    sleep 0.1
done
if [ "$READY" != "1" ]; then
    echo "面板启动失败，日志如下：" >&2
    cat "$RUN_DIR/monitor.log" >&2 || true
    exit 1
fi

if [ "$OPEN_BROWSER" = "1" ]; then
    if command -v wslview >/dev/null 2>&1; then
        wslview "$URL" >/dev/null 2>&1 &
    elif [ -n "${WSL_DISTRO_NAME:-}" ] && command -v cmd.exe >/dev/null 2>&1; then
        cmd.exe /c start "" "$URL" >/dev/null 2>&1 || true
    elif command -v xdg-open >/dev/null 2>&1; then
        xdg-open "$URL" >/dev/null 2>&1 || true
    fi
fi

echo "监测面板: $URL"
echo "运行目录: $RUN_DIR"
if [ "$AUTOSTART" = "none" ]; then
    echo "自动启动: 无（在页面中选择演示或测试）"
else
    echo "自动启动: $AUTOSTART 演示（页面内可切换、停止或运行测试）"
fi

wait "$WEB_PID"
