# 线格式与可靠性机制

## Event 数据处理顺序

发送端：业务序列化 → （启用断点续收时）加 32 位序号头 → E2E 保护 → 按 MTU 分片 → Backend 发送。

接收端：Backend 收包 → 分片重组 → E2E 校验 → （启用断点续收时）校验并剥离序号、去重 → 业务反序列化 → 用户回调。

序号头位于 E2E 之前，因此 CRC 覆盖序号本身；序号被篡改同样会被 E2E 拦下。

## 自定义分片头

```text
msg_id(2B, 大端) | total(1B) | index(1B) | payload_len(2B, 大端) | payload
```

约束：MTU 必须大于 6 字节；单条报文最多 255 个分片；重复分片不重复计数；总分片数或负载长度不一致时拒绝报文；支持乱序到达。

## 心跳与重连

服务端通过保留事件 `0x8002` 发送 `HB`。客户端超过配置的 timeout 未收到心跳后进入重连状态。`request_service()` 仅表示发现请求已提交，只有收到 `AVAILABLE` 回调才确认恢复，并重新订阅事件。

## 事件断点续收（项目自定义扩展）

> 事件在 ara::com / SOME/IP 里是"通知"语义，标准不提供丢包回补。这一节是项目
> 自行加的一层，用来补"客户端短暂断连"这一段，**不是标准行为**。

启用后（`services.json` 的 `resume` 段，默认关闭）：

- 每个 Event 携带一根 **32 位单调序号**，线格式为 `seq(4B, 大端) | <E2E 保护的负载>`。
- 发布端为每个 Event 保留最近 `buffer_size` 条（序号 + 业务负载）的内存环形缓存。
- 客户端记录每个 Event 已见过的最大序号；重连恢复后对每个已订阅事件发起一次续收。
- 续收通过一个**保留 Method** 完成：`RESUME_METHOD_ID = 0x7FFE`
  （Method 最高位为 0，与 Event 约定区分）。
  - 请求：`event_id(2B, 大端) | last_seq(4B, 大端)`；`last_seq = 0xFFFFFFFF` 表示"没有任何历史，把缓存里的都发来"。
  - 响应：`status(1B) | count(2B, 大端) | earliest(4B, 大端)`；`status = 0` 表示已按请求回放，`status = 1` 表示请求位置已早于缓存范围，**存在无法补齐的缺口**。
- 回放**保留原序号、重做 E2E**（E2E 计数器是新的），因此客户端能靠序号对回放与实时流去重。
- 服务端回放缓存是内存态的：**服务端重启后缓存失效，续收只能报缺口**。

## 同步 RPC 的"至多一次"（项目自定义扩展）

> 同步调用超时是二义的：`TIMEOUT` 不代表服务端没有执行。调用方一旦自行重试，
> 同一命令就可能执行两次。这一层把"重试 + 去重"配对提供——**重试不能裸加**。

启用后（`services.json` 的 `idempotency` 段，默认关闭）：

- 方法请求线格式变为 `request_key(8B, 大端) | 原请求负载`；接收端在业务反序列化**之前**剥掉 key。
- 客户端为**每次逻辑调用**生成一个 64 位 key；`call_method_sync_idempotent()` 在
  `TIMEOUT` / `NOT_AVAILABLE` 时用**同一个 key** 重试，其余错误立即上抛。
- 服务端为每个方法维护定长环形窗口 `key -> 响应`；命中直接返回缓存响应，**不重复执行 handler**。

**已知边界**：窗口是内存态且定长——超出窗口的旧 key 会被当作新请求重新执行；
不做在途互斥，同一 key 的并发请求可能都被执行（覆盖场景是"超时后顺序重试"）；
服务端重启窗口即失效。

## 同机共享内存通道的记录格式

这不是 SOME/IP 线上协议，只用于同机两个进程。每个方向一个共享内存环：

- 头部：magic `ARASHM01`、version、header_size、capacity、max_message、
  `creator_pid`、state，以及 `head` / `tail` 两个自由增长的字节计数；
- 下标是 `pos % capacity`，`tail - head` 就是已用空间，空/满没有二义性；
- 记录格式：`length(4B, 主机序) | flags(4B, 主机序) | payload | 8 字节对齐填充`；
  `flags = 1` 是回绕跳过标记；
- 容量尾部放不下一条记录时，先写跳过标记，消费者看到标记后跳到回绕边界；
- 生产者写完记录后 release 发布 `tail`，消费者 acquire 读取；长度越界或记录不完整时
  把 `state` 标成 `BROKEN`，不再继续读；
- 唤醒用命名信号量，接收侧用单调时钟超时；底层通道不做服务发现、方法路由或事件订阅。

单条消息上限默认 64 KiB，每个方向的容量默认 1 MiB；服务端和客户端必须配置一致。

## 共享内存 Backend 的控制帧

`ShmBackend` 在通道负载里再包一层固定 24 字节帧头，字段为主机序（只用于同机通信）：

| 偏移 | 长度 | 字段 |
| --- | --- | --- |
| 0 | 4 | magic `SHM1` |
| 4 | 2 | version，当前为 1 |
| 6 | 2 | type |
| 8 | 2 | service id |
| 10 | 2 | instance id |
| 12 | 2 | method id / event id |
| 14 | 2 | session |
| 16 | 2 | status（响应使用 `ComRet` 数值） |
| 18 | 2 | 保留 |
| 20 | 4 | payload_size |
| 24 | N | payload |

type 取值：

- `1 HELLO`：payload 为 `major(1B) | minor(4B) | 数据通道名`，服务端校验服务已发布且版本一致；
- `2 ACK`：payload 为空，表示客户端注册成功；
- `3 REQUEST` / `4 RESPONSE`：`id` 为 Method，`session` 用于匹配请求与响应；
- `5 EVENT`：`id` 为 Event，只发送给已订阅的客户端；
- `6 SUBSCRIBE` / `7 UNSUBSCRIBE`：`id` 为 Event，`session` 携带 Event Group；
- `8 BYE`：客户端退出或服务端停止发布时通知对端。

`payload_size` 必须等于总帧长减 24；单帧上限 64 KiB。客户端注册通过文件锁串行化，
服务端最多接受 32 个客户端。这里没有动态服务发现：客户端按固定服务名找到控制通道。

## 已知非标准部分

- 数据类型采用项目自定义序列化，不保证与 CommonAPI 生成代码兼容。
- 分片协议不是 SOME/IP-TP。
- CRC-8 未包含完整 AUTOSAR Data ID 等配置项。
- 事件的 32 位序号头与续收 Method（`0x7FFE`）是项目自定义扩展，不属于 ara::com / SOME/IP 标准。
- 方法请求的 8 字节请求 key 与"至多一次"去重窗口同样是项目自定义扩展。
- 同机共享内存环的头部、记录格式和唤醒方式为项目自定义，不属于 ara::com 标准。
- 共享内存 Backend 的 24 字节控制帧、HELLO/ACK 注册和固定服务名同样为项目自定义扩展。
