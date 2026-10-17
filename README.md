# ara-com-middleware

这是一个用 C++17 编写的多传输通信实验项目。业务通过 `ara::com` 风格的 Proxy/Skeleton
API 发布订阅事件、调用方法；底层可以选择 COVESA vsomeip、POSIX 共享内存或 eProsima
Fast DDS，并提供进程内测试后端。项目重点是通信抽象、超时、分片、重连与异常路径验证。

它不是 AUTOSAR Adaptive Platform 的完整实现，也没有做一致性或功能安全认证。
项目里的 E2E 和分片格式都是教学用途的简化实现，不能直接用于量产通信。

## 目前做到的内容

- `ServiceSkeleton`：服务发布、Event 注册/发送和 Method handler；
- `ServiceProxy`：服务发现、Event 订阅、同步/异步 Method 调用；
- 四种 backend：进程内 `NullBackend`、vsomeip backend、同机共享内存 `ShmBackend`，以及可选的 Fast DDS `DdsBackend`；
- CRC-8 + rolling counter、应用层分片、心跳和指数退避重连；
- 事件断点续收：32 位序号 + 发布端环形缓存 + 保留 Method 回放（项目自定义扩展，默认关闭）；
- 同步 RPC 至多一次：64 位请求 key + 服务端去重窗口，超时重试复用同一 key（项目自定义扩展，默认关闭）；
- JSON 配置、异步日志以及 GTest 测试；
- 同机共享内存通道：POSIX shared memory + 双向 SPSC 环形队列 + 命名信号量唤醒，
  覆盖回绕、满队列、陈旧对象清理和双进程压测；
- 共享内存 backend：多客户端注册、Method 路由和 Event 订阅广播，单条消息上限 64 KiB；
- DDS backend：真实 RTPS/UDP 发现、Topic 事件订阅、同步/异步请求应答、多客户端隔离和服务恢复；
- 运行监测与诊断：进程内原子指标、JSONL 结构化日志（按大小轮转与保留）、本机 Web 面板
  （DDS / SOME/IP 视图切换、演示启停与测试按钮）和一键启动脚本。

`src/ipc/shared_memory.*` 是最底层的同机字节通道；`ShmBackend` 在它之上实现
`ICommunicationBackend`，让 `ServiceProxy` / `ServiceSkeleton` 可以不改代码切换传输。
共享内存 backend 不做动态服务发现，客户端按固定服务名注册到控制通道。

## 代码结构

```text
src/communication/   Proxy、Skeleton 和 backend（含 ShmBackend、DdsBackend）
src/safety/          E2E、分片、心跳、重连
src/common/          配置解析、日志、监测指标和公共类型
src/ipc/             POSIX 共享内存通道（SPSC 环形队列）
demo/                车速服务示例（vsomeip 服务端 / 客户端）
monitor/             本机 Web 面板（单页 HTML，无第三方前端依赖）
tools/               演示与诊断工具（dds_rpc_pair、monitor_server.py）
launcher/            Windows .NET 8 启动器源码
scripts/             测试、演示与监测脚本（含 monitor_action.sh）
tests/               单元测试和并发测试
config/              服务与 vsomeip 配置
```

协议细节和实现限制见 [docs/protocol.md](docs/protocol.md)，测试边界见
[docs/run-all-tests.md](docs/run-all-tests.md)。

## 一键测试

根目录的 `run-tests.sh` 把配置、编译、测试和结果汇总串成一步：它只调用仓库里已有的
CMake 选项和 `scripts/` 下的脚本，不修改任何源码。Windows 下双击 `run-tests.cmd`
会在 WSL 中执行同一个脚本，并把参数原样转发过去。

```bash
bash run-tests.sh                 # 默认：unit + guards + shm + vsomeip（约 4 分钟）
bash run-tests.sh quick           # 只跑不依赖 vsomeip 的快速回归（约 1 分钟）
bash run-tests.sh all             # unit + guards + shm + asan + tsan + vsomeip
bash run-tests.sh shm             # 只跑同机共享内存通道
bash run-tests.sh vsomeip         # 只跑真实 SOME/IP 端到端
bash run-tests.sh recovery        # 稳定性：服务端停止后客户端恢复
bash run-tests.sh concurrency     # 并发压测：窗口 4 / 16
bash run-tests.sh capacity        # 容量/极限测试（约 21 分钟）
bash run-tests.sh unit --clean -j 8
bash run-tests.sh --help          # 完整用法
```

| 模式 | 内容 |
| --- | --- |
| `unit` | NullBackend 单元测试（GTest + ctest） |
| `guards` | 判定逻辑与失败归档的回归检查 |
| `shm` | 共享内存单测 + 裸通道双进程压测 + RPC 多客户端验证 |
| `dds` | 可选 Fast DDS 后端，全量回归 + RTPS/UDP 多进程验证 |
| `vsomeip` | 双进程链路、异步完成、多在途并发、停止恢复 |
| `recovery` | 稳定性：服务端停止后客户端恢复 |
| `concurrency` | 并发压测：窗口 4 / 16 的多在途调用 |
| `quick` | `unit` + `guards`，不依赖 vsomeip |
| `asan` | ASan + UBSan 构建并测试 |
| `tsan` | TSan 构建并测试 |
| `capacity` | vsomeip 容量测试 |

脚本按模式自动探测依赖：缺少 cmake、GTest、vsomeip3 或 `/usr/bin/time` 时，对应阶段
标记为“跳过”并说明安装方式，而不是直接报错。每个阶段的原始日志保存在
`artifacts/oneclick-时间戳/`，`vsomeip` 模式的双进程证据保存在
`tests/reports/staging/`，与手工执行时的归档约定一致。默认在第一个失败阶段后停止，
加 `--keep-going` 可以跑完所有阶段；退出码 0 表示没有失败阶段。

### 双进程分开看：服务端一个终端，客户端一个终端

上面的 `vsomeip` 模式是无人值守的自动编排（适合回归和 CI）。如果要像
[测试指南](docs/run-all-tests.md) 里那样直接观察两侧的日志，可以用"单侧"启动脚本：

```bash
# 终端一
bash scripts/vsomeip_pair.sh server demo
# 终端二
bash scripts/vsomeip_pair.sh client demo
```

Windows 下双击 `run-vsomeip.cmd`，它会直接开好这两个终端窗口：

```text
run-vsomeip.cmd                        默认 demo 用例，开服务端 + 客户端两个窗口
run-vsomeip.cmd completion             异步 RPC 完成与超时边界
run-vsomeip.cmd concurrency            多在途并发（默认 100 4）
run-vsomeip.cmd concurrency both 1000 16
run-vsomeip.cmd completion server      只开服务端（客户端由你手动再开）
```

两侧通过 `artifacts/vsomeip-pair/<用例>/ready` 就绪标记握手：服务端打印就绪标记后才
放行客户端，所以先启动哪一边都可以（客户端最多等 45 秒），也不会被上一轮遗留的标记
误导。日志分开放：`server.log` 和 `client.log`，客户端跑完会自动调用离线检查器并把
结论写进 `check.txt`。服务端窗口按 Ctrl+C 结束。

## 同机共享内存通道与 backend

`src/ipc/` 提供一条独立于 ara::com backend 的字节通道，用于同机两个进程之间的
高频消息传递。服务端创建 POSIX 共享内存和命名信号量，客户端按名字打开；
两个方向各一条 SPSC 环形队列，单条消息最大 64 KiB。

```bash
cmake -S . -B build-shm -DARA_COM_BUILD_SHM=ON -DARA_COM_USE_VSOMEIP=OFF
cmake --build build-shm -j"$(nproc)"

# 终端一
./build-shm/tools/shm_pair server demo_shm 100000
# 终端二
./build-shm/tools/shm_pair client demo_shm 100000 64 64
```

客户端输出 `RESULT total=... ok=... throughput_msg_s=... p99_us=...`。
自动验证覆盖小消息、64 KiB 消息、客户端先启动、服务端崩溃重启：

```bash
bash scripts/verify_shm.sh build-shm
```

### 通过 ShmBackend 跑 ara::com 调用

`ShmBackend` 实现 `ICommunicationBackend`，让 `ServiceProxy` / `ServiceSkeleton`
可以直接走共享内存。服务端创建控制通道，客户端注册后获得一条独立双向通道；
Method 请求按方法号路由，Event 只发送给已订阅的客户端，最多支持 32 个客户端。

```bash
# 终端一：服务端，service_id 0x7A01（十进制 31233），运行 20 秒
./build-shm/tools/shm_rpc_pair server 31233 20000

# 终端二、三：两个客户端，各 200 次 RPC
./build-shm/tools/shm_rpc_pair client 31233 1 200
./build-shm/tools/shm_rpc_pair client 31233 2 200
```

客户端输出 `CLIENT_RESULT id=... ok=... fail=... events=...`。自动验证：

```bash
bash scripts/verify_shm_backend.sh build-shm
# 或者
bash run-tests.sh shm
```

边界：共享内存 backend 不做动态服务发现，客户端按固定服务名注册；单服务端最多
32 个客户端，单条消息上限 64 KiB；每个客户端一条通道，方法处理在各自接收线程上
并发执行，业务 handler 需要自己保证线程安全。服务端正常退出会 unlink 共享内存和
信号量；异常退出后下一次启动按 `creator_pid` 清理陈旧对象。

## Fast DDS 后端

`DdsBackend` 将相同的 Proxy/Skeleton 接口映射到 DDS Topic。eProsima Fast DDS 提供
DDS/RTPS 协议栈；本项目实现服务发布状态、版本隔离、请求应答关联与超时收尾，不是 DDS-RPC 标准实现。

```bash
# Ubuntu 22.04：Fast DDS 2.5 与 Fast CDR
sudo apt-get install libfastrtps-dev libfastcdr-dev libgtest-dev
bash run-tests.sh dds
```

手动构建与运行：

```bash
cmake -S . -B build-dds -DARA_COM_USE_DDS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-dds -j4
ctest --test-dir build-dds --output-on-failure

# 终端一：service id 31233、Domain ID 61、运行 60 秒
./build-dds/tools/dds_rpc_pair server 31233 61 60000
# 终端二：同一 service/domain，客户端标识 1，200 次 RPC
./build-dds/tools/dds_rpc_pair client 31233 61 1 200
```

- 默认关闭 DDS，原有构建无需安装 DDS；显式开启但缺依赖时配置失败，不用跳过掩盖未测试。
- 每 backend 一个服务/实例/精确版本，同一服务一个活动提供者；Domain 默认 0，可通过
  `DdsBackendOptions{61}` 构造后端隔离通信域。
- 请求和应答使用两个 Reliable/Volatile Topic，GUID + 64 位请求 ID 隔离客户端；上限为
  128 个在途请求、64 KiB 业务 payload。业务回调异常和超长应答转为错误返回。
- 事件每个事件/组一个 Topic，Reliable/Volatile/KeepLast(32)；不自动向迟加入的订阅者回放历史。
- 跨进程强制 RTPS/UDP，禁用 DDS 共享内存优化；只在请求、应答端点都匹配后报告可用，
  停止发布与进程失联后重新发现。恢复匹配不代表补齐断线期间的事件。
- 服务端 handler 串行；回调不得同步等待同一 backend 的 RPC 或直接停止后端。
  没有 DDS Security、持久化、多提供者负载均衡与跨厂商互通保证。

## 运行监测与诊断

监测内核位于 `src/common/monitor.*` 和 `src/common/logger.*`，不依赖具体通信后端，也不依赖 GUI：

- `Monitor` 用原子计数器记录 RPC 提交、成功、超时、不可用、失败、延迟、在途数，Event 发送、接收、
  交付、重复、损坏、序号跳变与缺失，续收回放/缺口，重连/断开、心跳超时，以及 Method 处理与去重命中；
- `AsyncLogger` 保留原有纯文本控制台输出，文件可切换为 JSONL，并按 `rotate_bytes` 与 `max_files`
  做按大小轮转和保留；
- 指标快照先写临时文件再原子改名，面板读取时不需要和业务进程共享锁。

一键运行（WSL，需要 Fast DDS、vsomeip3 与 Python 3）：

```bash
bash scripts/monitor.sh
# 打开已有运行目录，不自动启动演示
bash scripts/monitor.sh --attach artifacts/monitor-时间戳
# 启动时不自动运行演示，全部由页面按钮触发
bash scripts/monitor.sh --autostart none
```

脚本默认自动启动一轮 DDS 演示，把指标与日志写到
`artifacts/monitor-时间戳/{metrics,logs}`，并打开 `http://127.0.0.1:8099/`。
页面顶部可在“全部 / DDS / SOME/IP”之间切换；运行控制区可以启动或停止 DDS、
SOME/IP 演示，按类型运行测试，并查看任务状态与动作日志。测试类型包括快速回归、单元、
判定逻辑、共享内存、SOME/IP、停止恢复、并发压测、DDS、ASan/UBSan、TSan、容量/极限和
全部测试；“全部测试”不含约 21 分钟的容量测试。演示时长可在 5–3600 秒之间调整，
用于长时间稳定性观察。
启动演示时 `scripts/monitor_action.sh` 会按需配置并构建 `build-monitor`；测试运行期间
会拒绝启动演示，避免共享内存和 SOME/IP 端口冲突。`--no-open`、`--port`、`--duration`、
`--requests`、`--clients` 可调整。

Windows 下可以直接双击启动，不必手输命令：

```cmd
launcher\build.cmd
```

首次会生成仓库根目录的 `ara-com-monitor.exe`（需要 .NET 8 SDK；运行需要 .NET 8 运行时）。
双击该 exe 后，它调用 WSL 执行同一个 `scripts/monitor.sh`，等面板就绪后再打开浏览器。
没有 .NET 的机器可以直接双击仓库根目录的 `启动监测.cmd`，效果相同。启动器只负责拉起
WSL 和打开页面，DDS、SOME/IP 与 Python 仍运行在 WSL 中；exe 是本地构建产物，不进入 Git。
这个启动器是为本地演示与排查提供便利的辅助入口，不属于通信中间件核心；真正的通信、
面板和测试都在 WSL 中运行。

面板接口：`/api/state?backend=dds|vsomeip` 返回按后端过滤的最新快照与汇总；
`/api/logs` 支持按后端、级别、进程、tag 和文本过滤；`/api/export.csv` 导出匹配日志；
`/api/actions` 提供任务状态和白名单动作。成功率按 `rpc_ok / rpc_submitted` 计算；
丢序/缺失来自事件序号跳变；续收回放是断连恢复后服务端按缓存补发的条数；平均延迟按已完成
调用计算，包含超时和失败。同一角色只展示最新一份快照，避免重复启动导致累计翻倍。

动作接口只接受固定动作：启动/停止 DDS 或 SOME/IP 演示、按类型运行/停止测试。POST 必须带
`X-ARA-Monitor: 1` 请求头和 JSON 内容类型，后端、测试类型与数值参数都会校验，不接收任意命令。
演示进程运行在独立进程组中，停止时按进程组终止。容量/极限测试约 21 分钟，页面会在启动前
二次确认。

演示工具通过 `ARA_COM_LOG_DIR`、`ARA_COM_METRICS_DIR`、`ARA_COM_METRICS_INTERVAL_MS` 等环境变量
显式开启日志与快照；不设置时保持原有行为。埋点位于 Proxy/Skeleton，DDS 与 vsomeip 共用同一套指标；
JSONL 日志带 `backend` 字段，面板据此过滤。

边界：面板只绑定 `127.0.0.1`，没有鉴权、TLS、WebSocket 和数据库，不是生产 APM；自定义请求头
只能阻止普通跨站表单提交，不替代本机权限边界。DDS 与 SOME/IP 演示已接入日志与快照，SHM 演示
尚未接入；日志轮转的磁盘满、目录权限变化、多进程写同一文件未测试；面板依赖 Python 3 标准库。

## 构建

建议在 Linux / WSL2 中构建和运行；真实 vsomeip 测试需要 Ubuntu 22.04 / WSL2 环境。
默认构建不依赖 vsomeip；如果系统没有 GTest，CMake 会跳过测试。
运行测试前请确认 CMake 已找到 GTest。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

也可以使用仓库里的构建脚本：

```bash
bash build.sh                 # Debug + NullBackend
bash build.sh release         # Release + NullBackend
bash build.sh x86 vsomeip     # 启用真实 SOME/IP backend
```

启用 vsomeip 时需要系统能够找到 `vsomeip3`：

```bash
cmake -S . -B build-vsomeip -DARA_COM_USE_VSOMEIP=ON
cmake --build build-vsomeip -j"$(nproc)"
```

## 运行示例

跨进程示例可以使用 vsomeip、`ShmBackend` 或 `DdsBackend`。`NullBackend` 只在
同一进程内路由消息，用于 API 单元测试；共享内存与 DDS 示例分别见
`tools/shm_rpc_pair.cpp`、`tools/dds_rpc_pair.cpp`，下面的命令演示 vsomeip 方式。

```bash
export VSOMEIP_CONFIGURATION="$PWD/config/vsomeip.json"

# terminal 1
export VSOMEIP_APPLICATION_NAME=ara_com_server_4660
./build-vsomeip/demo/demo_server config/services.json

# terminal 2
export VSOMEIP_APPLICATION_NAME=ara_com_client_4660
./build-vsomeip/demo/demo_client config/services.json
```

`scripts/verify_vsomeip.sh` 封装了双进程检查，使用 `build/` 下的程序，并把日志保存到 `artifacts/vsomeip-validation-时间戳/`。完整命令见测试指南。

## 测试与运行时修复

- 异步 RPC 正确性：完成次数、超时边界、迟到响应，以及窗口 4/16 的多在途请求验证。
- 服务停止恢复：通过 SIGTERM 停止服务端并重启，检查可用性变化及恢复后的 RPC。
- 容量测试：真实 vsomeip 后端，同机独立 router/server/client，64 字节 Echo，单客户端在途窗口 1/4/16/32/64/128；每档预热 10 秒、测量 60 秒、重复 3 次。窗口数不是独立客户端数量。
- 同机共享内存：10 万条 64 B 消息在 WSL2 上约 250 万条/秒、p99 约 40 µs；2000 条 64 KiB 消息约 1.0–1.25 GB/s、p99 约 0.8–1.0 ms（单次实测，随机器变化）。覆盖回绕、满队列、客户端先启动和服务端 `kill -9` 后的重启清理。
- 默认构建（DDS 关闭）：92 个 CTest 项（73 个基础单元测试 + 12 个共享内存通道测试 + 4 个 ShmBackend 测试 + 3 个观测测试）；Release 与 TSan 均为 92/92，ASan/UBSan 由 107 项配置覆盖。含 SHM 的完整套件按串行方式运行，避免并行用例抢同名共享内存通道。
- DDS backend：WSL Release 与 ASan/UBSan 均通过 107 个 CTest 项（默认 92 个 + 14 个 DDS GTest + 1 个多进程编排）；编排验证双客户端各 200 次 RPC、事件广播及 SIGKILL 服务端后的原客户端恢复，连续五轮通过。
- 观测组件：`Monitor` 计数/快照与 `AsyncLogger` JSONL/轮转由独立测试进程覆盖；DDS+SHM 的 Release 与 ASan/UBSan 均为 107/107。默认构建 TSan 92/92，`tests/tsan.supp` 只抑制 libtsan 对 `wait_for` 的已知 double-lock 误报；DDS 仍不包含在 TSan 证据中。
- 运行时修复：保存服务发布时的 major/minor 版本，停止发布时使用相同版本，并避免重复停止。

复现步骤见[测试运行指南](docs/run-all-tests.md)。
测试指南末尾列出了运行记录需要保存的信息；原始报告单独保存。
当前 CI 默认关闭 DDS，运行 92 个单元测试（含共享内存通道、backend 与观测组件）及 ASan/UBSan。
DDS 用例只在显式启用时注册到 CTest；真实 vsomeip 集成与容量测试需按指南手动执行，未接入 CTest/CI。

## 没有覆盖的范围

- 完整的 ara::com API 和代码生成流程；
- AUTOSAR E2E Profile 兼容性；
- SOME/IP-TP；
- 共享内存 backend 的动态服务发现：目前按固定服务名注册，不做运行时发现；
- 网络管理、持久化和安全认证；
- 监测面板的鉴权、TLS、跨机访问与历史数据库：当前是本机演示面板，控制接口不替代权限边界，
  不能当作生产 APM；
- 日志轮转在磁盘满、目录权限变化、多进程写同一文件下的行为未测试。

这些边界也是后续继续完善这个项目时最值得展开的部分。
