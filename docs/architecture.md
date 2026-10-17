# 架构设计说明

## 项目边界

本项目是基于 COVESA vsomeip 的 ara::com 风格原型框架，不是 AUTOSAR AP 一致性实现，也未经过 ISO 26262 功能安全认证。它重点展示通信抽象、服务化 API、可靠性机制与可测试性。

## 分层

```text
业务应用
  ├─ ServiceSkeleton：服务提供、Event 发布、Method 处理
  └─ ServiceProxy：服务发现、Event 订阅、同步/异步 Method 调用
          ↓
ICommunicationBackend：隔离业务 API 与传输实现
  ├─ VSomeIPBackend：真实 SOME/IP/SOME/IP-SD
  └─ NullBackend：进程内确定性测试替身
          ↓
E2E / 分片重组 / 心跳 / 重连 / 日志 / 配置

独立于 Backend 的一条同机通路：
SharedMemoryChannel：双向 SPSC 环形队列 + 命名信号量唤醒
ShmBackend：基于 SharedMemoryChannel 的 ICommunicationBackend 实现
```

## 关键取舍

- Backend 抽象让上层测试不依赖网络环境，但 NullBackend 不能证明真实网络性能与协议兼容性。
- 自定义分片用于研究大报文处理，不等同于标准 SOME/IP-TP。
- E2E 只实现 CRC-8 与循环计数器的学习用途简化子集，不宣称完整 Profile 1 兼容。
- 每个 Event 使用独立检查器和重组器，避免不同事件的状态互相污染。
- 异步 RPC 使用单一超时调度线程，避免高并发下“一请求一线程”。

## 同机共享内存通道

`src/ipc/` 是底层字节通道，本身不参与服务发现、方法路由和事件订阅：

- 服务端到客户端、客户端到服务端各一条 SPSC 环形队列，放在 POSIX 共享内存里；
- `head`/`tail` 是自由增长的字节计数，下标为 `pos % capacity`，空/满没有二义性；
- 容量尾部放不下一条变长记录时，先写 8 字节跳过标记，再从 0 继续；
- 生产者写完记录后 release 发布 `tail`，消费者 acquire 读取，避免读到半条记录；
- 跨进程唤醒用命名信号量，接收侧使用单调时钟超时并一次排空；
- 服务端 `O_EXCL` 创建，客户端按超时重试打开；启动时按 magic/version/`creator_pid`
  判断并清理陈旧对象；
- 进程内 `send`/`receive` 各有一把互斥锁串行化同进程多线程调用；跨进程仍是单生产者单消费者。

跨进程共享内存的竞争不会被单进程 TSan 跟踪到，因此正确性证据来自双进程逐条校验、
回绕/满队列边界用例和崩溃重启用例，见 `scripts/verify_shm.sh`。

### ShmBackend：把通道接到 ara::com API

`src/communication/shm_backend.*` 在通道之上实现 `ICommunicationBackend`：

- 服务端创建 `<svc>_<sid>_<iid>.ctl` 控制通道，客户端串行发送 HELLO 注册；
  服务端打开客户端预先创建的数据通道并回 ACK，之后数据只走这条独立通道；
- 客户端用文件锁串行化注册，数据通道名包含进程号和进程内递增序号，避免重名；
- Method 请求携带 `(sid, iid, mid, session)`，服务端按方法号查 handler，
  响应从原客户端通道返回；同一客户端的多条请求按会话号匹配；
- Event 只发送给已发送 SUBSCRIBE 的客户端，`send_event` 遍历在线连接；
- 客户端连接线程负责注册、断连检测和重连；服务端后台线程定期清理已退出客户端；
- 单服务端最多 32 个客户端，单条消息上限 64 KiB，不做动态服务发现。

服务端方法 handler 在对应客户端的数据接收线程上执行，不同客户端之间可以并发；
用户 handler 需要自行保证线程安全。

## 线程安全约束

- 用户回调必须在框架内部锁释放后执行。
- 分片器的消息编号和重组缓存均受互斥锁保护。
- vsomeip 收包线程只负责路由，耗时业务处理应由上层转交线程池。
- Proxy/Skeleton 销毁前必须先停止心跳、重连和后端事件循环。
- 共享内存通道的接收回调由独立线程调用，且不持有通道内部锁；停止时 join。
- 共享内存通道析构前先停止接收线程，再由创建者 unlink 共享内存和信号量。
- `ShmBackend` 服务端每个客户端一条接收线程；方法 handler 不持后端锁执行。
- `ShmBackend` 客户端先停后端（含接收线程），再销毁 Proxy，避免回调落到已析构对象。
