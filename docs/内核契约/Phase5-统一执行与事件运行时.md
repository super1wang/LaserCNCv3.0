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

## 本阶段后续工作

- 实现 CommandRuntime 的 Schema -> Capability -> Revision Precondition -> ApplicationTransaction -> Handler -> Result Schema -> Commit -> EventBus -> Log 完整链；
- 实现只基于不可变快照的 QueryRuntime；
- 实现并发安全、有限容量的内存幂等记录，重复请求返回原结果且不重复提交/发事件；持久化幂等留给 Phase 8；
- 接入 AppKernel 冻结与生命周期；
- 建立无领域代码的 Headless/CLI 测试入口，验证发现、命令执行、查询执行、日志与 CTest 共用同一 Runtime。
