#!/usr/bin/env bash

set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${project_dir}/build"
run_stamp="$(date +%Y%m%d-%H%M%S)"
artifact_dir="${project_dir}/artifacts/vsomeip-validation-${run_stamp}"
server_bin="${build_dir}/demo/demo_server"
client_bin="${build_dir}/demo/demo_client"
service_config="${project_dir}/config/services.json"
vsomeip_config="${project_dir}/config/vsomeip.json"

for required in "${server_bin}" "${client_bin}" "${service_config}" "${vsomeip_config}"; do
    if [[ ! -e "${required}" ]]; then
        echo "缺少文件：${required}" >&2
        echo "请先使用 -DARA_COM_USE_VSOMEIP=ON 完成构建。" >&2
        exit 2
    fi
done

mkdir -p "${artifact_dir}"

server_pid=""
cleanup() {
    if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" 2>/dev/null; then
        kill -TERM "${server_pid}" 2>/dev/null || true
        wait "${server_pid}" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

export VSOMEIP_CONFIGURATION="${vsomeip_config}"

echo "启动服务端……"
(
    export VSOMEIP_APPLICATION_NAME="ara_com_server_4660"
    exec "${server_bin}" "${service_config}"
) >"${artifact_dir}/server.stdout.log" 2>&1 &
server_pid=$!

sleep 2
if ! kill -0 "${server_pid}" 2>/dev/null; then
    echo "服务端提前退出，请检查 ${artifact_dir}/server.stdout.log" >&2
    exit 1
fi

echo "启动客户端，采集 8 秒通信日志……"
set +e
(
    export VSOMEIP_APPLICATION_NAME="ara_com_client_4660"
    timeout --signal=TERM 8s "${client_bin}" "${service_config}"
) >"${artifact_dir}/client.stdout.log" 2>&1
client_status=$?
set -e

if [[ ${client_status} -ne 0 && ${client_status} -ne 124 ]]; then
    echo "客户端异常退出，状态码 ${client_status}" >&2
    exit 1
fi

required_patterns=("Service available" "Event received" "Sync RPC result" "Async RPC completed")
for pattern in "${required_patterns[@]}"; do
    if ! grep -Fq "${pattern}" "${artifact_dir}/client.stdout.log"; then
        echo "验证失败：客户端日志缺少 '${pattern}'" >&2
        exit 1
    fi
done

{
    echo "commit=$(git -C "${project_dir}" rev-parse HEAD 2>/dev/null || echo uncommitted)"
    echo "date=$(date --iso-8601=seconds)"
    echo "kernel=$(uname -srmo)"
    echo "compiler=$(${CXX:-c++} --version | head -n 1)"
    echo "cmake=$(cmake --version | head -n 1)"
} >"${artifact_dir}/environment.txt"

echo "真实 vsomeip 双进程基本链路验证通过。"
echo "证据目录：${artifact_dir}"
