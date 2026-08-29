# 可复现测试指南

## 本地单元测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DARA_COM_BUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## Sanitizer

```bash
cmake -S . -B build-asan -DARA_COM_ENABLE_ASAN=ON -DARA_COM_ENABLE_UBSAN=ON
cmake --build build-asan -j$(nproc)
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DARA_COM_ENABLE_TSAN=ON
cmake --build build-tsan -j$(nproc)
ctest --test-dir build-tsan --output-on-failure
```

## Docker

```bash
docker build -t ara-com-demo .
```

## 真实 vsomeip 验证

安装 COVESA vsomeip 后执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DARA_COM_USE_VSOMEIP=ON -DARA_COM_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
chmod +x scripts/verify_vsomeip.sh
./scripts/verify_vsomeip.sh
```

脚本启动 Server/Client 两个进程，检查服务可用、Event、同步 RPC 和异步 RPC 日志，并将环境信息及两端输出保存到 `artifacts/vsomeip-validation/`。该目录默认不提交 Git；需要公开测试证据时，应人工检查日志后选择性添加脱敏结果。

只有真实后端集成测试可以证明跨进程 SOME/IP 与服务发现路径，NullBackend 数据不能替代网络指标。协议层验证还应保存 vsomeip 版本和 Wireshark 抓包。
