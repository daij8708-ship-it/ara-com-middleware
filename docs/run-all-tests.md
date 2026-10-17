# 测试准备与运行入口

真实 vsomeip 测试在 Ubuntu 22.04 / WSL2 中运行。请先安装 CMake >=3.20、C++17 编译器、GoogleTest、vsomeip3 开发库、Python3、bash、GNU coreutils 和 /usr/bin/time。

请在项目根目录执行以下命令。一次只运行一套真实后端测试，避免相同 application ID 和 routing socket 冲突。推荐复制到 WSL Linux 文件系统构建。测试脚本用 bash 启动，不依赖 Windows 保存执行权限。

## 单元测试（不依赖 vsomeip）

默认使用进程内 `NullBackend`，检查 E2E、分片重组、JSON 配置、Proxy/Skeleton 接口、多服务并发，以及心跳和重连线程的停止行为。它同步分发消息，不模拟网络延迟、丢包或跨进程隔离。

```bash
cmake -S . -B build-unit -DCMAKE_BUILD_TYPE=Release -DARA_COM_BUILD_TESTS=ON -DARA_COM_USE_VSOMEIP=OFF
cmake --build build-unit -j4
ctest --test-dir build-unit --output-on-failure
```

确认配置输出找到 GTest，且 CTest 实际发现并执行了测试。

测试判定与脚本失败归档的回归检查（需要 Python3、bash 和 g++，不启动真实服务）：

```bash
python3 tests/test_release_guards.py
```

## 同机共享内存通道（不依赖 vsomeip）

共享内存通道使用 POSIX `shm_open` / `mmap` 和命名信号量，不需要 vsomeip、router 或应用名。
Linux 下 `ARA_COM_BUILD_SHM` 默认开启；Windows 下默认关闭，`shared_memory.cpp` 会编成
返回失败的桩。

```bash
cmake -S . -B build-shm -DCMAKE_BUILD_TYPE=Release \
  -DARA_COM_BUILD_SHM=ON -DARA_COM_USE_VSOMEIP=OFF -DARA_COM_BUILD_TESTS=ON
cmake --build build-shm -j4
ctest --test-dir build-shm --output-on-failure
```

该构建共 92 个测试：73 个基础单元测试、12 个通道测试、4 个 backend 测试、3 个观测测试。
含 SHM 的完整套件请用 `ctest -j1` 或默认串行方式运行：部分用例使用固定的共享内存通道名，
并行执行会互相抢占并报 `already in use`，这不是通信代码失败。
通道测试在同一进程里打开服务端/客户端两个句柄，覆盖边界长度、回绕、满队列、超时、
回调线程、客户端先启动、陈旧对象清理、析构清理、配置校验和非法名字。
backend 测试覆盖单客户端 RPC/Event、两个客户端的路由与广播、异步回复/超时，
以及服务端重启后的自动重连。

双进程验证：

```bash
bash scripts/verify_shm.sh build-shm
bash scripts/verify_shm_backend.sh build-shm
# 或者
bash run-tests.sh shm
```

`verify_shm.sh` 的四个用例：10 万条 64 B 消息、2000 条 64 KiB 消息、客户端先启动、
服务端 `kill -9` 后客户端退出且重启能清理陈旧对象。客户端输出 `RESULT` 行，
脚本核对 `ok == total`；证据保存在 `artifacts/shm-validation-时间戳/`。

`verify_shm_backend.sh` 启动一个服务端和两个客户端，每个客户端执行 200 次 RPC
并订阅 Event；脚本核对两个客户端都满足 `fail=0`、`events>0`，证据保存在
`artifacts/shm-backend-时间戳/`。

跨进程竞争无法用单进程 TSan 跟踪，正确性由逐条数据校验、回绕/满队列边界、
崩溃重启用例和双进程 RPC 验证保证。ASan/UBSan 与 TSan 均运行全部 92 个测试；
TSan 通过 `setarch -R` 关闭 ASLR，并用 `tests/tsan.supp` 抑制 libtsan 对
`condition_variable::wait_for` 的已知 double-lock 误报，数据竞争检测不受影响。

## Fast DDS 后端（显式启用）

Ubuntu 22.04 安装 `libfastrtps-dev`、`libfastcdr-dev`、`libgtest-dev`，验证组合为
Fast DDS 2.5.0 / Fast CDR 1.0.23。数据走 RTPS/UDP，未启动 vsomeip router，DDS 的共享
内存 transport 与 Data Sharing 均关闭。

```bash
bash run-tests.sh dds
# 或手动执行
cmake -S . -B build-dds -DARA_COM_USE_DDS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-dds -j4
ctest --test-dir build-dds --output-on-failure
ctest --test-dir build-dds -L dds --output-on-failure
bash scripts/verify_dds_backend.sh build-dds
```

启用 DDS 和 SHM 共 107 个 CTest 项：默认 92 个、14 个 DDS GTest、1 个多进程编排。
关闭 DDS 仍为 92 个；不要把默认 CI 的 92 个说成已经覆盖 DDS。

DDS GTest 验证真实 Participant/Reader/Writer，而不是替身：

- DDS 工厂与 Domain 参数校验、客户端先启动、未 offer 不可用；
- Proxy/Skeleton 的 RPC 与事件、双客户端并发应答隔离与广播；
- 单客户端 8 线程混合同步/异步请求，共 128 次调用，服务端单串行工作线程；
- 超时后迟到应答只完成一次、应答回调再提交异步请求、停止时唤醒在途调用；
- 停止发布再发布、服务端重建后自动恢复 RPC 与事件，不重新创建订阅；
- Domain、service、instance、major/minor 精确隔离；
- 65536 字节 payload 成功、65537 字节拒绝、超长应答和业务异常返回错误；
- 128 个在途占满后拒绝第 129 个、排空后复用容量；
- 事件 ID/组隔离、Volatile 不回放迟加入前的历史。

`DDS.MultiProcess` 启动一个服务端、两个客户端，各 200 次请求（同步/异步各 100），
逐条校验客户端编号与序号，核对回调数、重复数、事件内容以及服务端总处理数 400。
第二阶段先等待客户端收到事件且初始 RPC 成功，再对自起服务端发送 SIGKILL；
等待客户端报告 UNAVAILABLE 后重启服务端，检查原客户端的 RPC 和事件均恢复。
结果与退出码保存在 `artifacts/dds-backend-时间戳/`，缺少结果或任意失败均非零退出。

本地 WSL Release 与 ASan/UBSan 均通过 107/107。DDS 的 TSan 未作为通过证据：
发行版 Fast DDS/Fast CDR 二进制没有同步进行 sanitizer 插桩，不能把默认后端的
TSan 结果扩展成 DDS 的线程安全结论。未验证跨机器、跨厂商互通、网络丢包注入、
DDS Security 和长期压力性能。

```bash
cmake -S . -B build-dds-asan -DARA_COM_USE_DDS=ON \
  -DARA_COM_ENABLE_ASAN=ON -DARA_COM_ENABLE_UBSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-dds-asan -j4
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-dds-asan --output-on-failure
```

## Sanitizer

ASan/UBSan 用于检查内存越界、释放后访问和未定义行为；TSan 用于检查线程竞争。两者使用独立构建目录，不能组合启用。

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DARA_COM_BUILD_TESTS=ON \
  -DARA_COM_USE_VSOMEIP=OFF -DARA_COM_ENABLE_ASAN=ON -DARA_COM_ENABLE_UBSAN=ON
cmake --build build-asan -j4
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DARA_COM_BUILD_TESTS=ON \
  -DARA_COM_USE_VSOMEIP=OFF -DARA_COM_ENABLE_TSAN=ON
cmake --build build-tsan -j4
ctest --test-dir build-tsan --output-on-failure
```

## Docker 构建

安装 Docker 后，在项目根目录执行：

```bash
docker build -t ara-com-demo .
```

## 基础 vsomeip 集成验证

检查独立 Server/Client 进程间的服务发现、Event 和同步/异步 RPC：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DARA_COM_USE_VSOMEIP=ON -DARA_COM_BUILD_TESTS=ON
cmake --build build -j4
bash scripts/verify_vsomeip.sh
```

脚本保存环境信息和两端日志到 `artifacts/vsomeip-validation-时间戳/`，该目录默认由 Git 忽略。协议层排查可结合 vsomeip 版本信息和 Wireshark 抓包。

## 异步完成与多在途正确性

```bash
cmake -S . -B build-rpc-trace -DCMAKE_BUILD_TYPE=Release -DARA_COM_USE_VSOMEIP=ON -DARA_COM_RPC_COMPLETION_TRACE=ON
cmake --build build-rpc-trace -j4
```

完成次数/超时边界测试，在终端一启动服务端：

```bash
VSOMEIP_CONFIGURATION="$PWD/config/vsomeip.json" VSOMEIP_APPLICATION_NAME=ara_com_server_4660 ./build-rpc-trace/rpc_completion_test server
```

终端二运行客户端并离线检查日志：

```bash
VSOMEIP_CONFIGURATION="$PWD/config/vsomeip.json" VSOMEIP_APPLICATION_NAME=ara_com_client_4660 timeout 180s ./build-rpc-trace/rpc_completion_test client 100 > completion-client.log 2>&1
echo "client_exit=$?"
python3 scripts/check_rpc_completion_trace.py completion-client.log
```

结束上述服务端后，另测多在途：终端一改为 `./build-rpc-trace/rpc_concurrency_test server`，仍使用相同两个环境变量；终端二：

```bash
VSOMEIP_CONFIGURATION="$PWD/config/vsomeip.json" VSOMEIP_APPLICATION_NAME=ara_com_client_4660 timeout 180s ./build-rpc-trace/rpc_concurrency_test client 100 4 > concurrency-w4.log 2>&1
echo "client_exit=$?"
python3 scripts/check_rpc_concurrency_trace.py concurrency-w4.log
VSOMEIP_CONFIGURATION="$PWD/config/vsomeip.json" VSOMEIP_APPLICATION_NAME=ara_com_client_4660 timeout 180s ./build-rpc-trace/rpc_concurrency_test client 1000 16 > concurrency-w16.log 2>&1
echo "client_exit=$?"
python3 scripts/check_rpc_concurrency_trace.py concurrency-w16.log
```

保留客户端退出码和检查器退出码，正常结束服务端后再运行其他测试。

## 服务停止与恢复

```bash
cmake -S . -B build-rpc-disconnect -DCMAKE_BUILD_TYPE=Release -DARA_COM_USE_VSOMEIP=ON -DARA_COM_RPC_COMPLETION_TRACE=OFF
cmake --build build-rpc-disconnect -j4
bash scripts/run_rpc_disconnect_recovery.sh
```

脚本自动启动独立 router/server/client，对服务端发送 SIGTERM 并重启，验证平稳停止后的恢复。异常断网和强制终止不在此用例范围内。输出保存在 `tests/reports/staging/` 下的新目录。

一键入口也可以只跑这一类：

```bash
bash run-tests.sh recovery      # 只跑服务停止与恢复
bash run-tests.sh concurrency   # 只跑窗口 4 / 16 的并发压测
```

`recovery` 复用 `build-rpc-disconnect`，`concurrency` 复用 `build-rpc-trace`；二者都要求
vsomeip3 与 GTest。容量/极限测试仍使用 `bash run-tests.sh capacity`，约 21 分钟。

## 运行监测面板（手动）

观测测试随默认构建执行，单独一个测试进程，覆盖指标计数与 JSON 快照、快照线程的启动/停止与
原子写、JSONL 日志格式以及按大小轮转和保留。

一键演示：

```bash
bash scripts/monitor.sh --no-open --duration 60 --requests 200
# 终端会打印面板地址，默认 http://127.0.0.1:8099/
# 只打开已有运行目录，不自动启动演示
bash scripts/monitor.sh --attach artifacts/monitor-时间戳
```

验证点：

- `artifacts/monitor-时间戳/metrics/` 下每个进程一份 `metrics-<role>-<pid>.json`；
- `artifacts/monitor-时间戳/logs/` 下每行一个 JSON 对象，包含 ts、level、backend、component、tag、pid、msg；
- `/api/state` 中 `rpc_submitted`、`rpc_ok` 与工具输出的 `CLIENT_RESULT` 对得上；
- `/api/state?backend=dds` 与 `?backend=vsomeip` 分别只返回对应后端的进程；
- `/api/logs?level=WARN&backend=vsomeip` 与 `/api/export.csv` 能按后端和级别过滤、导出；
- 页面“运行控制”可以启动/停止两种演示；测试运行期间启动演示返回 409；
- 测试类型下拉框覆盖快速回归、单元、判定逻辑、共享内存、SOME/IP、停止恢复、并发压测、
  DDS、ASan/UBSan、TSan、容量/极限和全部测试；“容量/极限”与“全部测试”启动前有二次确认；
- 选择“快速回归”后运行测试，动作日志能看到 ctest 与判定检查输出；选择“停止恢复”或
  “并发压测”能单独启动对应阶段，不需要跑完整 `vsomeip` 模式；
- 演示时长输入框支持 5–3600 秒，可用于长时间稳定性观察；超过 3600 秒会被接口拒绝；
- 不带 `X-ARA-Monitor: 1` 的 POST 返回 403，未知动作、测试类型和后端参数被拒绝；
- 客户端退出后最后一份快照带 `"stopped": true`，面板显示“已结束”而不是“不可用”；
- Ctrl+C 退出后没有残留的 `dds_rpc_pair`、`demo_server`、`demo_client` 或 `monitor_server.py` 进程。

面板只绑定 `127.0.0.1`，没有鉴权、TLS 和历史数据库；控制接口虽只接受白名单动作并要求自定义
请求头，但不替代本机权限边界。DDS 与 SOME/IP 演示已接入日志与快照，SHM 演示尚未接入。
日志轮转在磁盘满、目录权限变化、多进程写同一文件下的行为未测试。

## 正式容量测试（约 21 分钟以上）

```bash
cmake -S . -B build-vsomeip-capacity -DCMAKE_BUILD_TYPE=Release -DARA_COM_USE_VSOMEIP=ON -DARA_COM_RPC_COMPLETION_TRACE=OFF
cmake --build build-vsomeip-capacity -j4
bash scripts/run_vsomeip_capacity.sh
```

窗口 1/4/16/32/64/128，每档 10 秒预热、60 秒测量、重复 3 次。脚本使用上述固定构建目录。不要用开启追踪的构建测容量。断线或人为中断时，保留原始日志，不把未完成轮次计入成功。

容量程序和离线检查器均要求零错误、零超时、请求全部成功完成，并达到指定在途窗口。脚本保存每轮程序退出码；某轮失败时停止后续轮次，检查器标记为未运行。检查器实际执行后，无论成功或失败，均记录检查器与日志写入命令的退出码。

## 测试结果与版本记录

每次测试单独记录以下信息，缺失项标为“未记录”：

- 测试时间、范围和拓扑，例如同机单客户端、多在途。
- `git rev-parse HEAD` 和 `git status --short --untracked-files=all`。有未提交修改时，另存包含未跟踪源码的快照和 SHA-256。
- Ubuntu/WSL、CPU/内存、编译器、CMake 和 vsomeip 版本。
- 构建命令、CMake 选项、配置文件与二进制的 SHA-256；测试期间保持源码和二进制不变。
- 实际运行命令、故障注入方式、各程序及检查器退出码、原始日志位置。
- 监测演示的运行目录、`metrics/*.json` 与 `logs/*.jsonl` 样本、面板接口输出，以及进程退出后的 `stopped` 标记。
- 成功、失败、跳过或中断的结果；补测另记，保留原来的失败记录。

命令执行后立即保存退出码。使用管道时，分别记录程序、检查器和日志写入命令的状态。

运行脚本在 `tests/reports/staging/` 创建独立结果目录，整个 `tests/reports/` 目录由 Git 忽略。归档时保留原始日志、退出码、源码与二进制指纹及 SHA-256 清单；公开前检查敏感信息。

各项结果对应记录中的被测版本。离线检查用于核对日志；源码发生行为变化后，应重新运行相关测试。归档时间与测试时间分开记录。

## CI 覆盖范围

GitHub Actions 运行 NullBackend 普通构建和 ASan/UBSan 测试，并上传 `test-results.xml`。对应提交的 Actions 页面可查看编译输出、失败用例和测试产物。真实后端集成与容量测试手动运行，未接入 CTest/CI。
