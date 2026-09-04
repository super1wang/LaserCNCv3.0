# Phase 5 统一执行与事件运行时

状态：Phase 5 历史检查点已验收；当前增量收口以 [冻结审计清单](Kernel-1.0-冻结审计清单.md) 为准，不将以下历史阶段范围自动扩张为最终 Kernel Frozen。

Phase 5 建立所有 Headless、CLI、未来 GUI/Script/AI 必须共用的 Command、Query 与 Event Kernel 入口。本阶段不实现任何 CAD、CAM、Machine、Process 或其他领域命令。

## 已建立的注册与安全基座

- CommandName、QueryName、RequestId、SessionId、CorrelationId、TraceId、IdempotencyKey、SubscriptionId 均为独立 StrongId。
- CommandRegistry 和 QueryRegistry 按稳定名称唯一注册，发现结果按名称确定性排序；后续由 AppKernel 在 Ready 边界冻结。
- CommandDescriptor 公开版本、参数/结果 Schema、执行模式、副作用、Capability、Undo、确定性和幂等元数据。
- Phase 5 只接受同步 `DocumentWrite` 命令；Phase 6 已在同一 CommandRuntime 上增加只读异步任务接受路径。Undo 仍须等待 Phase 8 Journal，文件发布与硬件副作用不得伪装成已具备事务保障。
- CapabilityService 对未知 Session 和未授予 Capability 默认拒绝，只支持显式精确授权；通配、角色继承和远端认证不在本阶段假设中。
- ExecutionServices 只持有 Kernel 的 `ISchemaValidator` 与 `ILogService` 端口；具体 jsoncons/spdlog 实现由组合根注入，公共 API 不出现第三方类型。

## 已建立的 EventBus 基座

- Domain Event、Notification、System Event 三类语义保持分离。
- Domain Event 只能由成功提交产生的 CommittedDomainEvent 转换；Notification 和 System Event 使用受限工厂创建，不能伪装成 Domain Event。
- 支持按事件类别/名称过滤、Immediate/Queued delivery、RAII Subscription lifetime、Trace/Correlation 传播。
- Notification 合并在 C6b4 收紧为同一订阅实例、事件名、完整 Version 和 coalescing key；Domain/System Event 不合并。取消后公开订阅 ID 的复用不能继承旧排队项，见 [C6b4 契约](ST1C6b4-消息准入与订阅身份.md)。
- 发布先在锁内准备投递快照，回调始终在锁外执行；回调可以重入发布，单个订阅者异常被转换为 delivery failure，不阻断其他订阅者，也不改变已提交业务状态。
- queued 事件由调用方显式 drain；EventBus 不私建线程，也不把 Phase 6 的长任务 Scheduler 冒充 Host 事件循环。

## 已建立的 CommandRuntime

命令固定执行链为：

`Registry -> Argument Schema -> Idempotency Contract -> Capability -> Project Revision -> ApplicationTransaction -> Handler -> Result Schema -> Commit -> EventBus -> Structured Log -> Response`

- CommandRuntime 只在 AppKernel Ready 状态接受请求，关闭后拒绝新执行；
- Request 的 ProjectId 必须与 Document 所属项目一致，`expectedRevision` 映射为 ProjectRevision 前置条件；
- Handler 异常转换为 Kernel Error，Handler failure 或结果 Schema failure 会 rollback staging state；
- commit 成功后，EventBus 或日志失败只进入 `postExecutionErrors`，不得把已提交命令反转成失败，防止无幂等键调用方误重试；字段在 Phase 6 扩展为同步提交与异步接受共用的后置集成诊断容器；
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

## Headless/CLI 进程契约

`integration.kernel_headless_cli_roundtrip` 从独立进程启动测试专用组合根，并通过参数选择固定 roundtrip 模式。它验证：

- Command/Query descriptor 发现；
- JSON 文本经 jsoncons Adapter 进入 Kernel Value；
- 参数和结果 Schema 使用真实 jsoncons backend；
- CommandRuntime 完成 Capability、ExpectedRevision、事务、事件和幂等链；
- QueryRuntime 从提交后的不可变快照读取；
- spdlog 生成非空 JSONL，最终结果重新序列化到标准输出。

该可执行文件只在 `LCNC_BUILD_TESTING=ON` 时生成，测试 handler 只位于 `tests/integration`。它证明 CLI 形态的进程调用与 Headless 调用共用同一 Runtime，不是生产 CLI Host，也没有暴露通用对象写命令。用户限定的内核阶段内不新增 CLI11 或上层模块；真正的产品 CLI 只负责适配协议，未来不得另建执行通道。

## 阶段边界

- 异步 Command、TaskRuntime、Scheduler、Cancellation 和资源仲裁已由 Phase 6 契约承接；
- Trace 目前只传播 TraceId，Trace/Metrics/Diagnostics 实现属于 Phase 7；
- 幂等持久化、Event Journal、Snapshot 和 Crash Recovery 属于 Phase 8；
- Workflow/Script 属于 Phase 9；
- CAD、CAM、Machine、Process、Qt GUI、产品 CLI、RPC 和 AI 等上层/Host 不在本阶段实现。
