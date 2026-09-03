# Phase 5 统一执行与事件运行时

状态：进行中。

Phase 5 建立所有 Headless、CLI、未来 GUI/Script/AI 必须共用的 Command、Query 与 Event Kernel 入口。本阶段不实现任何 CAD、CAM、Machine、Process 或其他领域命令。

## 已建立的注册与安全基座

- CommandName、QueryName、RequestId、SessionId、CorrelationId、TraceId、IdempotencyKey、SubscriptionId 均为独立 StrongId。
- CommandRegistry 和 QueryRegistry 按稳定名称唯一注册，发现结果按名称确定性排序；后续由 AppKernel 在 Ready 边界冻结。
- CommandDescriptor 公开版本、参数/结果 Schema、执行模式、副作用、Capability、Undo、确定性和幂等元数据。
- Phase 5 只接受同步 `DocumentWrite` 命令；异步执行必须等待 Phase 6 TaskRuntime，Undo 必须等待 Phase 8 Journal，文件发布与硬件副作用不得伪装成已具备事务保障。
- CapabilityService 对未知 Session 和未授予 Capability 默认拒绝，只支持显式精确授权；通配、角色继承和远端认证不在本阶段假设中。
- ExecutionServices 只持有 Kernel 的 `ISchemaValidator` 与 `ILogService` 端口；具体 jsoncons/spdlog 实现由组合根注入，公共 API 不出现第三方类型。

## 已建立的 EventBus 基座

- Domain Event、Notification、System Event 三类语义保持分离。
- Domain Event 只能由成功提交产生的 CommittedDomainEvent 转换；Notification 和 System Event 使用受限工厂创建，不能伪装成 Domain Event。
- 支持按事件类别/名称过滤、Immediate/Queued delivery、RAII Subscription lifetime、Trace/Correlation 传播。
- Notification 可按订阅、事件名和 coalescing key 合并；Domain/System Event 不合并。
- 发布先在锁内准备投递快照，回调始终在锁外执行；回调可以重入发布，单个订阅者异常被转换为 delivery failure，不阻断其他订阅者，也不改变已提交业务状态。
- queued 事件由调用方显式 drain；Phase 6 之前 EventBus 不私建线程、不借用三方线程池冒充 Scheduler。

## 已建立的 CommandRuntime

命令固定执行链为：

`Registry -> Argument Schema -> Idempotency Contract -> Capability -> Project Revision -> ApplicationTransaction -> Handler -> Result Schema -> Commit -> EventBus -> Structured Log -> Response`

- CommandRuntime 只在 AppKernel Ready 状态接受请求，关闭后拒绝新执行；
- Request 的 ProjectId 必须与 Document 所属项目一致，`expectedRevision` 映射为 ProjectRevision 前置条件；
- Handler 异常转换为 Kernel Error，Handler failure 或结果 Schema failure 会 rollback staging state；
- commit 成功后，EventBus 或日志失败只进入 `postCommitErrors`，不得把已提交命令反转成失败，防止无幂等键调用方误重试；
- 幂等记录在内存中并发安全且容量有限。相同 key 与相同业务签名共享一个 in-flight 执行，重试返回原 commit 并标记 replayed，不重新执行、不重复发事件；不同业务签名绑定同一 key 会冲突；
- 业务签名包含 Session、Project、Document、Command、Arguments 和 ExpectedRevision，不包含每次传输可变化的 RequestId、CorrelationId、TraceId；
- 当前幂等记录不是持久化 exactly-once 保证，进程重启后的恢复与淘汰策略持久化属于 Phase 8。

## 已建立的 QueryRuntime

- QueryRuntime 固定执行 Registry、Argument Schema、Capability、Snapshot、Handler、Result Schema、Structured Log；
- `requiresDocument` 查询必须携带 DocumentId；Document 所属 Project 必须匹配 Request；
- Handler 只获得按值不可变 Document 快照，返回快照 RevisionSet；不获得 Transaction 或 DocumentStore 可变入口；
- 日志失败进入 `postExecutionErrors`，不改变成功查询结果；
- Query 不使用幂等记录，因为它不得产生业务副作用。

## AppKernel 生命周期

- AppKernel 拥有 ExecutionServices、CapabilityService、EventBus、Command/Query Registry 和 Command/Query Runtime；
- 模块完成注册后，AppKernel 在 Ready 边界冻结 ExecutionServices 和两个 Registry，再开启执行入口；
- 已注册 Command/Query 但未配置 Schema/Log 端口时 bootstrap fail-closed，并清理已启动模块；
- shutdown 前检查活动 Transaction、Command 和 Query；生命周期本身不支持并发驱动。

## 本阶段剩余工作

- 建立无领域代码的 Headless/CLI 测试入口，验证发现、命令执行、查询执行、jsoncons Schema、spdlog 日志与 CTest 共用同一 Runtime；
- 补齐完整 Release、重复执行、production-only 和架构边界门禁，再更新阶段交付文档。
