# 架构设计说明

## 项目边界

本项目在 COVESA vsomeip、eProsima Fast DDS 与自研共享内存通道之上提供 ara::com 风格 API，不是 AUTOSAR AP 一致性实现，也未经过 ISO 26262 功能安全认证。它重点展示通信抽象、服务化 API、可靠性机制与可测试性。

## 分层

```text
业务应用
  ├─ ServiceSkeleton：服务提供、Event 发布、Method 处理
  └─ ServiceProxy：服务发现、Event 订阅、同步/异步 Method 调用
          ↓
ICommunicationBackend：隔离业务 API 与传输实现
  ├─ VSomeIPBackend：真实 SOME/IP/SOME/IP-SD
  ├─ ShmBackend：同机共享内存、多客户端路由
  ├─ DdsBackend：Fast DDS、RTPS/UDP、请求应答与事件 Topic
  └─ NullBackend：进程内确定性测试替身
          ↓
E2E / 分片重组 / 心跳 / 重连 / 日志 / 监测指标 / 配置

可独立使用，也由 ShmBackend 复用的同机字节通路：
SharedMemoryChannel：双向 SPSC 环形队列 + 命名信号量唤醒
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

## DDS 后端

`DdsBackend` 是第四种 `ICommunicationBackend` 实现，不改变 Proxy/Skeleton 的调用接口。
`ARA_COM_USE_DDS=OFF` 时不查找或链接 DDS 依赖；开启时要求 Fast DDS 2.5 及以上的 2.x
与 Fast CDR。未启用时请求创建 DDS 后端返回空指针，不静默退回 NullBackend。

- 每个 backend 创建一个 Participant，绑定一个 service/instance/精确版本；Domain ID 可独立指定。
- `start()` 只启动基础实体；`offer_service()` 创建请求 Reader 和应答 Writer，停止发布时删除这两个端点。
- 客户端必须同时匹配请求 Reader 和应答 Writer 才通知可用；仅发现 Participant 不算服务上线。
- 同一服务限定一个活动提供者，发现多个匹配端点时不接受调用；不实现多提供者选择或负载均衡。
- Event 每个事件/组一个 Topic，底层按订阅匹配；请求和应答共享两个 Topic，GUID 与 64 位 ID 防串台。
- 应用消息装入有界 CDR 信封；业务 payload 上限 64 KiB，DDS 不替代原有业务序列化。
- 应用层每个 backend 一个收包线程，客户端另有一个超时线程，均以 5ms 周期检查；Fast DDS 自身另有传输线程。
- pending 表的单次移除决定谁有权完成请求；超时、应答、断连、停止都经过同一张表，迟到应答被丢弃。
- 禁用 DDS SHM transport 与 Data Sharing，独立进程数据走 RTPS/UDP，避免混淆与自研共享内存后端的验证。
- Participant 租约为 3s、公告周期 1s；异常退出依赖租约失效，不承诺立即探知或硬实时恢复。

Event 使用 Reliable/Volatile/KeepLast(32)，RPC 使用 Reliable/Volatile/KeepAll，样本资源上限 128。
Writer 心跳显式设为 50ms，配合 100ms 有界写等待，避免默认 3s 确认周期造成缓存提前耗尽。
选择 Volatile 是为了不自动重放旧命令；Reliable 只负责传输可靠性，不替代业务幂等或事件续收。
没有 DDS Security、持久化、任意 QoS 配置、标准 DDS-RPC 和跨厂商互通保证。

## 运行监测与日志

监测层与具体 backend 解耦，埋点放在 Proxy/Skeleton 的公共路径上：

- `src/common/monitor.*` 是进程级单例，所有计数用原子变量更新；Proxy 记录 RPC 结果、延迟、
  在途数、事件接收/重复/损坏、序号跳变、续收和可用性变化，Skeleton 记录事件发送、
  Method 处理/异常和去重命中。
- 快照是单行 JSON，包含进程标签、服务号、PID、启动/更新时间、计数器与派生值；写入时先写
  `.tmp` 再原子改名，读侧无需加锁。
- `src/common/logger.*` 的控制台仍输出纯文本，文件可切换 JSONL；写线程按单文件上限轮转，
  只保留指定数量的历史文件。队列满时丢弃并计数，不阻塞业务线程。
- 快照写线程与日志写线程各自只操作自己的状态；指标更新不进入业务锁，避免监测反向影响通信路径。
- `monitor/index.html` 与 `tools/monitor_server.py` 通过 `/api/state`、`/api/logs` 和
  `/api/export.csv` 读取运行目录，不改变线上协议，也不被库链接。
- JSONL 日志带 `backend` 字段，`/api/state` 与 `/api/logs` 支持按后端过滤；DDS 角色使用
  `dds-*` 前缀，vsomeip 角色使用 `vsomeip-*` 前缀，避免同名进程互相覆盖快照。
- `/api/actions` 只接受白名单动作：启动/停止 DDS 或 SOME/IP 演示、运行/停止测试。
  动作由 `scripts/monitor_action.sh` 在独立进程组中执行，面板退出时按进程组终止；
  同一后端只允许一个演示，测试与演示互斥。POST 需要 `X-ARA-Monitor: 1` 请求头。
- 同一角色只展示最新快照；进程退出时最后一份快照带 `stopped=true`，面板据此区分“不可用”和“已结束”。

## 线程安全约束

- 用户回调必须在框架内部锁释放后执行。
- DDS 服务端 handler 在收包线程串行执行；应答、事件和可用性回调也不能阻塞该线程。
  可从回调发起异步调用，但不要在回调内同步等待同一后端的 RPC 或执行 `stop()`。
- DDS 停止后端会 join 应用工作线程并取消在途调用；停止可能等待当前业务 handler 返回。
  先停 Proxy/Skeleton 的心跳重连，再停 backend，最后销毁 Proxy/Skeleton，避免悬空回调。
- 分片器的消息编号和重组缓存均受互斥锁保护。
- vsomeip 收包线程只负责路由，耗时业务处理应由上层转交线程池。
- Proxy/Skeleton 销毁前必须先停止心跳、重连和后端事件循环。
- 共享内存通道的接收回调由独立线程调用，且不持有通道内部锁；停止时 join。
- 共享内存通道析构前先停止接收线程，再由创建者 unlink 共享内存和信号量。
- `ShmBackend` 服务端每个客户端一条接收线程；方法 handler 不持后端锁执行。
- `ShmBackend` 客户端先停后端（含接收线程），再销毁 Proxy，避免回调落到已析构对象。
- 监测计数只用原子操作；快照线程停止时先释放内部锁，再写最后一份快照；日志轮转只发生在
  日志写线程内，不阻塞调用 `log()` 的业务线程。
