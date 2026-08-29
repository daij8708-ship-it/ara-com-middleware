# 测试策略

本文说明测试范围和复现方式，不手工记录容易随代码变化而失效的“通过数量”。每个提交的真实结果以 GitHub Actions 日志和测试产物为准。

## 测试分层

### 单元测试

默认使用 `NullBackend`，用于确定性验证：

- E2E CRC、计数器、篡改和截断检测；
- 分片往返、乱序、重复、非法头、超时清理和容量边界；
- JSON 配置解析及默认值；
- Proxy/Skeleton 生命周期、Event 和 Method API；
- 多服务并发调用；
- 心跳和重连线程的停止及状态确认语义。

NullBackend 在调用线程内同步分发消息，不模拟网络延迟、丢包、SOME/IP-SD 状态机或跨进程隔离。

### Sanitizer

CI 使用 ASan/UBSan 检查内存越界、释放后访问和未定义行为。TSan 构建选项供 Linux 本地检查线程竞争；由于 TSan 与 ASan/UBSan 不能组合，应使用独立构建目录。

### 真实后端集成测试

真实 `VSomeIPBackend` 验证需要 Linux 和 COVESA vsomeip，目标包括：

- 两个独立进程完成服务发现；
- Event 发布/订阅；
- 同步和异步 RPC；
- 心跳接收；
- 服务退出、重新启动后的可用性变化与重新订阅。

仓库中的 `scripts/verify_vsomeip.sh` 用于启动两端、保存日志并检查关键输出。该脚本通过并不等同于 AUTOSAR 一致性或功能安全认证。

## 本地运行

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DARA_COM_BUILD_TESTS=ON -DARA_COM_USE_VSOMEIP=OFF
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

## CI 结果

GitHub Actions 对普通构建和 ASan/UBSan 构建分别运行测试，并上传 `test-results.xml`。查看某次提交的 Actions 页面即可核实编译器输出、失败用例和测试产物。
