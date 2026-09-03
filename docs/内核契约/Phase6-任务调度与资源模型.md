# Phase 6 任务调度与资源模型契约

## 阶段目标

Phase 6 在 Kernel 内建立长耗时工作的唯一语义入口：`TaskRuntime -> Scheduler -> ITaskExecutor -> BsThreadPoolExecutor`。前三层的任务身份、状态、依赖、取消、进度、截止时间和资源仲裁由 LaserCNC 自研；BS::thread_pool 继续只是可替换的执行 backend。本阶段不实现任何 CAD、CAM、Machine、Process、Qt GUI、产品 Host 或真实领域长任务。

## 注册与状态

- TaskName 与 TaskId 是不同 StrongId；TaskDescriptor 携带版本、输入 Schema 和结果 Schema。
- TaskRegistry 只在组合期接受唯一注册，AppKernel Ready 前冻结；空 handler、重复名称和冻结后注册均 fail-closed。
- 状态集合固定为 Pending、Ready、Running、Succeeded、Failed、CancelRequested、Cancelled、Stale。
- 依赖未完成时为 Pending；依赖全部成功后进入 Ready；任一依赖未成功则进入 Stale。
- Scheduler 在相同优先级内按提交顺序 FIFO。资源阻塞只阻塞该候选，不得饿死其他资源不冲突的 Ready 任务。

## 统一任务上下文

每次执行只能从 TaskContext 获得：

- CancellationToken：只读协作取消信号；令牌同时感知显式取消和 Deadline；
- ProgressReporter：只接受有限数值、范围 `[0, 1]` 和单调不回退的进度；
- TraceId：Phase 6 只传播，不实现 Trace backend；
- ResourceContext：执行前已原子获得的声明快照；
- 可选 Document：提交时复制的不可变文档快照。

任务 handler 不得持有活动 Document 可变引用，也不得自行创建 `stop`、`quit`、`abortFlag` 等第二套取消协议。取消 Pending/Ready 任务直接进入 Cancelled；取消 Running 任务先进入 CancelRequested，handler 退出后进入 Cancelled。非协作 handler 不会被不安全地强制终止。

## 文档与修订

- 文档任务必须同时声明 ProjectId 和 DocumentId；TaskRuntime 校验项目归属并在提交时捕获 Document 与六域 RevisionSet。
- 可选 expectedRevisions 与提交时快照不一致时拒绝入队。
- handler 只看到捕获快照。任务完成时若当前文档修订已变化，结果进入 Stale，并返回 `Task.SourceRevisionChanged`。
- Stale 检测是后台结果的第一道保护，不授予写权限。未来把结果应用到活动文档时仍必须进入 Command/ApplicationTransaction，并再次执行 Revision precondition。

## Resource Model

资源类别固定为 CPU、DiskIO、GPU、OCCT、ProjectRead、ProjectWrite、MachineController、CollisionBackend。每个声明包含稳定 ResourceId、Shared/Exclusive 模式和正整数 units。

- 一个任务的全部声明全有或全无获取，不能持有部分资源等待其余资源；
- Shared 使用受配置 capacity 约束，Exclusive 要求该槽无任何持有者；
- ProjectRead 与 ProjectWrite 规范化到同一项目槽，ProjectWrite 强制 Exclusive；
- 未显式配置的资源实例容量默认为 1；组合期可配置容量，Scheduler start 后冻结；
- 任务结束、失败、取消或 executor 提交失败都必须释放已持有资源。

## 异步 Command 接入

异步入口仍是 Phase 5 建立的同一个 CommandRuntime，不允许 GUI、CLI、Script 或未来 RPC 直接绕过命令链创建领域任务。

- CommandRegistry 通过 `registerAsyncHandler` 显式区分 `ICommandHandler` 与 `IAsyncCommandHandler`，handler 类型和 ExecutionMode 不一致时拒绝注册；
- Phase 6 异步 Command 仅允许 `ReadOnly`，负责接受后台计算，不允许获得 ApplicationTransaction 或声明 DocumentWrite/文件发布/硬件副作用；
- CommandRuntime 在准备任务前统一执行参数 Schema、Capability 和内存幂等检查；
- IAsyncCommandHandler 只返回 Task 计划与接受结果。CommandRuntime 校验接受结果 Schema，并强制把原 Command 的 ProjectId、DocumentId、ExpectedRevision、TraceId、CorrelationId 写入 TaskRequest，不能信任 handler 自带的跨边界上下文；
- 接受成功返回 `CommandResponse.taskId` 且 `commit` 为空；同步事务成功返回 `commit` 且 `taskId` 为空。二者的后置 Event/Log 失败统一记录到 `postExecutionErrors`，不反转已经提交或接受的结果；
- 同一 IdempotencyKey 和相同业务签名重试时返回原 TaskId，不重复准备或提交任务。持久化幂等仍属于 Phase 8。

## 生命周期与关闭

AppKernel 在 Configuring 注入并独占 `ITaskExecutor`，模块在组合回调中注册任务与配置资源。存在已注册任务但没有 Executor 或 ExecutionServices 时，bootstrap 失败。Ready 后 TaskRegistry、ResourceManager 和 ExecutionServices 同步冻结。

关闭顺序为：停止 Command/Query/Task 接收、向全部非终态任务请求取消、等待运行任务在给定时限内协作退出、关闭 Executor、再停止模块。时限到达返回 `Task.ShutdownTimeout`，Kernel 保持 Stopping，可在任务退出后重试；不得伪报 Stopped。

## 当前错误契约

| 范围 | 代表错误码 |
|---|---|
| 注册与身份 | `Task.InvalidHandler`、`Task.AlreadyRegistered`、`Task.RegistryFrozen`、`Task.IdAlreadyExists` |
| 依赖 | `Task.SelfDependency`、`Task.DuplicateDependency`、`Task.DependencyNotFound`、`Task.DependencyDidNotSucceed` |
| 进度与取消 | `Task.InvalidProgress`、`Task.ProgressRegression`、`Task.Cancelled`、`Task.DeadlineExceeded` |
| 文档与修订 | `Task.ProjectRequired`、`Task.ProjectMismatch`、`Task.RevisionConflict`、`Task.SourceRevisionChanged` |
| 资源 | `Task.InvalidResourceCapacity`、`Task.InvalidResourceUnits`、`Task.ConflictingResourceClaims` |
| 异步命令 | `Command.HandlerModeMismatch`、`Command.AsyncSideEffectUnsupported`、`Command.PostAcceptanceIntegrationFailed` |
| 生命周期 | `Task.ExecutorNotConfigured`、`Task.RuntimeNotAccepting`、`Task.WaitTimeout`、`Task.ShutdownTimeout` |

## 验收与后续边界

Phase 6 已完成 Debug/Release 全量 CTest、Debug 连续重复、Production-only、架构扫描与独立进程闭环。Queued Event 继续由调用方显式 drain：Scheduler 是长任务语义，不冒充 GUI/Host 事件循环；后续 Host 可以经明确的内核适配驱动 drain，但不得为 EventBus 私建无生命周期线程。Trace/Metrics/Diagnostics 属于 Phase 7；持久化任务、Journal、Snapshot、Crash Recovery 属于 Phase 8；Workflow/Script 属于 Phase 9；所有上层领域模块继续延后。
