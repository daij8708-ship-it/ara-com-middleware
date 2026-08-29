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
```

## 关键取舍

- Backend 抽象让上层测试不依赖网络环境，但 NullBackend 不能证明真实网络性能与协议兼容性。
- 自定义分片用于研究大报文处理，不等同于标准 SOME/IP-TP。
- E2E 只实现 CRC-8 与循环计数器的学习用途简化子集，不宣称完整 Profile 1 兼容。
- 每个 Event 使用独立检查器和重组器，避免不同事件的状态互相污染。
- 异步 RPC 使用单一超时调度线程，避免高并发下“一请求一线程”。

## 线程安全约束

- 用户回调必须在框架内部锁释放后执行。
- 分片器的消息编号和重组缓存均受互斥锁保护。
- vsomeip 收包线程只负责路由，耗时业务处理应由上层转交线程池。
- Proxy/Skeleton 销毁前必须先停止心跳、重连和后端事件循环。
