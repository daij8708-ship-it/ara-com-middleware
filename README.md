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
- JSON 配置、异步日志以及 GTest 测试。

`src/ipc/shared_memory.*` 目前只是验证接口用的进程内队列。它保留了早期 IPC
实验的接口，但并没有使用 POSIX shared memory。

## 代码结构

```text
src/communication/   Proxy、Skeleton 和 backend
src/safety/          E2E、分片、心跳、重连
src/common/          配置解析、日志和公共类型
src/ipc/             进程内 IPC 接口原型
demo/                车速服务示例
tests/               单元测试和并发测试
config/              服务与 vsomeip 配置
```

协议细节和实现限制见 [docs/protocol.md](docs/protocol.md)，测试边界见
[docs/run-all-tests.md](docs/run-all-tests.md)。

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

跨进程的 server/client 示例必须使用 vsomeip backend。`NullBackend` 只在同一进程
内路由消息，用于 API 单元测试。

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
- 运行时修复：保存服务发布时的 major/minor 版本，停止发布时使用相同版本，并避免重复停止。

复现步骤见[测试运行指南](docs/run-all-tests.md)。
测试指南末尾列出了运行记录需要保存的信息；原始报告单独保存。
当前 CI 运行 NullBackend 单元测试及 ASan/UBSan；真实后端集成与容量测试需按指南手动执行，未接入 CTest/CI。

## 没有覆盖的范围

- 完整的 ara::com API 和代码生成流程；
- AUTOSAR E2E Profile 兼容性；
- SOME/IP-TP；
- 跨进程共享内存传输；
- 网络管理、持久化和安全认证。

这些边界也是后续继续完善这个项目时最值得展开的部分。
