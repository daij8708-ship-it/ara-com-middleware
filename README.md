# ara-com-middleware

这是一个用 C++17 写的 SOME/IP 通信实验项目。我在 vsomeip 上封装了一层接近
`ara::com` 使用方式的 Proxy/Skeleton API，主要用来熟悉服务发现、事件和方法调用，
并练习通信层的超时、分片与重连处理。

它不是 AUTOSAR Adaptive Platform 的完整实现，也没有做一致性或功能安全认证。
项目里的 E2E 和分片格式都是教学用途的简化实现，不能直接用于量产通信。

## 目前做到的内容

- `ServiceSkeleton`：服务发布、Event 注册/发送和 Method handler；
- `ServiceProxy`：服务发现、Event 订阅、同步/异步 Method 调用；
- 两种 backend：用于单元测试的进程内 `NullBackend`，以及可选的 vsomeip backend；
- CRC-8 + rolling counter、应用层分片、心跳和指数退避重连；
- 事件断点续收：32 位序号 + 发布端环形缓存 + 保留 Method 回放（项目自定义扩展，默认关闭）；
- 同步 RPC 至多一次：64 位请求 key + 服务端去重窗口，超时重试复用同一 key（项目自定义扩展，默认关闭）；
- JSON 配置、异步日志以及 GTest 测试；
- 同机共享内存通道：POSIX shared memory + 双向 SPSC 环形队列 + 命名信号量唤醒，
  覆盖回绕、满队列、陈旧对象清理和双进程压测。

`src/ipc/shared_memory.*` 是独立的同机字节通道，不实现 `ICommunicationBackend`，
也不参与服务发现和方法路由。

## 代码结构

```text
src/communication/   Proxy、Skeleton 和 backend
src/safety/          E2E、分片、心跳、重连
src/common/          配置解析、日志和公共类型
src/ipc/             同机共享内存通道（SPSC 环形队列）
demo/                车速服务示例
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
bash run-tests.sh capacity        # 容量测试（约 21 分钟）
bash run-tests.sh unit --clean -j 8
bash run-tests.sh --help          # 完整用法
```

| 模式 | 内容 |
| --- | --- |
| `unit` | NullBackend 单元测试（GTest + ctest） |
| `guards` | 判定逻辑与失败归档的回归检查 |
| `shm` | 共享内存通道单测 + 双进程吞吐/崩溃恢复验证 |
| `vsomeip` | 双进程链路、异步完成、多在途并发、停止恢复 |
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

## 同机共享内存通道

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
# 或者
bash run-tests.sh shm
```

边界：通道只负责字节消息，不做服务发现、方法路由或事件订阅；当前是单服务端单客户端，
未接入 `ICommunicationBackend`。服务端正常退出会 unlink 共享内存和信号量；异常退出后
下一次启动会按 `creator_pid` 清理陈旧对象。

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

跨进程的 ara::com server/client 示例必须使用 vsomeip backend。`NullBackend` 只在同一
进程内路由消息，用于 API 单元测试；共享内存通道是另一条独立的同机字节通道，不参与
下面的 ara::com 示例。

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
- 运行时修复：保存服务发布时的 major/minor 版本，停止发布时使用相同版本，并避免重复停止。

复现步骤见[测试运行指南](docs/run-all-tests.md)。
测试指南末尾列出了运行记录需要保存的信息；原始报告单独保存。
当前 CI 运行单元测试（含共享内存通道）及 ASan/UBSan；真实后端集成与容量测试需按指南手动执行，未接入 CTest/CI。

## 没有覆盖的范围

- 完整的 ara::com API 和代码生成流程；
- AUTOSAR E2E Profile 兼容性；
- SOME/IP-TP；
- 共享内存通道接入 `ICommunicationBackend`：当前只提供字节通道，不支持服务发现、方法路由或多客户端；
- 网络管理、持久化和安全认证。

这些边界也是后续继续完善这个项目时最值得展开的部分。
