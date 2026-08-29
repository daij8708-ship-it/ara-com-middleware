# 线格式与可靠性机制

## Event 数据处理顺序

发送端：业务序列化 → E2E 保护 → 按 MTU 分片 → Backend 发送。

接收端：Backend 收包 → 分片重组 → E2E 校验 → 业务反序列化 → 用户回调。

## 自定义分片头

```text
msg_id(2B, 大端) | total(1B) | index(1B) | payload_len(2B, 大端) | payload
```

约束：MTU 必须大于 6 字节；单条报文最多 255 个分片；重复分片不重复计数；总分片数或负载长度不一致时拒绝报文；支持乱序到达。

## 心跳与重连

服务端通过保留事件 `0x8002` 发送 `HB`。客户端超过配置的 timeout 未收到心跳后进入重连状态。`request_service()` 仅表示发现请求已提交，只有收到 `AVAILABLE` 回调才确认恢复，并重新订阅事件。

## 已知非标准部分

- 数据类型采用项目自定义序列化，不保证与 CommonAPI 生成代码兼容。
- 分片协议不是 SOME/IP-TP。
- CRC-8 未包含完整 AUTOSAR Data ID 等配置项。
