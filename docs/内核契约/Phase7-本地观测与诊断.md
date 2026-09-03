# Phase 7 本地观测与诊断契约

## 阶段目标

Phase 7 建立 Kernel 自有的 Trace、Metrics 与 Diagnostics 语义，并确保观测失败不改变 Command、Query、Task 或 Transaction 的业务控制流。本阶段已完成本地服务、exporter/check 端口、关键执行链接入和多配置/独立进程验收。

当前不引入 OpenTelemetry SDK，不把 SQLite 当作日志或 Trace backend，也不实现远程诊断、MES、数字孪生、Cloud 或任何上层模块。

## Trace

- ITraceService 创建 ITraceSpan；TraceId 表示完整链路，SpanId 表示单个执行区间，可选 parentSpanId 建立父子关系。
- Span 状态为 Running、Succeeded、Failed、Cancelled、Stale；同一 SpanId 在活动和保留窗口内不可重用。
- Span 必须显式 end；句柄销毁前未完成时自动记录 `Trace.SpanAbandoned`，不能静默丢失。
- LocalTraceService 先在锁内完成本地记录，再复制 exporter 快照并在锁外调用；exporter 返回失败或抛异常只进入有界 exporterFailures，不反转 Span 或业务结果。
- 完成记录采用有界 FIFO 保留。容量至少为 1，默认记录 4096 条、exporter 失败 256 条。
- AppKernel 拥有 LocalTraceService，并在 Ready 边界冻结 exporter 组合；运行期可读取完成记录、活动数量和 exporter 失败快照，但不能改变组合。
- `command.execute`、`query.execute` 和实际进入 Executor 的 `task.execute` 各自形成 Span。请求可携带 parentSpanId；异步 Command 生成的 Task 强制继承 TraceId，并以当前 Command Span 为父节点。
- Task 的成功、失败、取消和源修订陈旧分别结束为 Succeeded、Failed、Cancelled、Stale。调度前已取消、依赖失败或 Deadline 已过的任务没有执行区间，因此不生成虚假的 `task.execute` Span。

## Metrics

- 支持 Counter、Gauge、Histogram；MetricName 与有序 labels 共同构成 series 身份。
- 同一 series 不得切换 MetricKind；所有观测必须为有限数值，Counter delta 不得为负。
- Counter 保存累计值，Gauge 保存最新值，Histogram 保存 count/sum/min/max；快照按稳定 MetricName/labels 顺序输出。
- exporter 在本地聚合完成后于锁外调用；失败仅进入有界 exporterFailures，记录方法仍成功。
- series 数量有界，默认 1024；达到容量后新 series 返回 `Metrics.SeriesCapacityExceeded`。运行时接入必须忽略该观测错误，不得影响业务结果。
- 默认 labels 禁止放入 TaskId、RequestId、ObjectId 等无界高基数身份；这些身份属于 Trace/Log 上下文。
- Command、Query、Task 均记录 `kernel.<kind>.completed` Counter 和 `kernel.<kind>.duration_ms` Histogram，默认只使用有界 `outcome` 标签；任何记录失败均被运行时吞并，不能覆盖业务结果。

## Diagnostics

- DiagnosticId 是稳定 StrongId；IDiagnosticCheck 只在组合期唯一注册，Ready 边界冻结。
- run 与 runAll 在注册表锁外执行 check，check 可以安全读取 latest 快照。
- check 返回 Error、抛异常或返回错误 ID 时转换为当前 DiagnosticId 的 Unhealthy 报告，而不是让诊断调用崩溃。
- runAll 按 DiagnosticId 确定性执行；latest 只保存每项最近报告。
- Diagnostics 报告状态为 Healthy、Degraded、Unhealthy、Unknown，不等同于 AppKernel 生命周期状态，也不能自行执行恢复动作。
- AppKernel 拥有 DiagnosticsService，并在 Ready 边界冻结 check 注册；具体 check 仍由组合层按实际 Host/Adapter 能力注册，内核不虚构硬件、网络或领域健康状态。

## Exporter 与持久化边界

ITraceExporter 与 IMetricsExporter 是稳定扩展端口。Local 服务始终先保留有界内存事实，再通知 exporter。LogObservabilityExporter 同时实现两个端口，只依赖 ILogService，将完成 Span 写为 `trace.span`，将单次 Metric 写为 `metric.observation`；使用 SpdlogLogService 的 JSONL sink 即可获得机器可读输出。该 exporter 显式在组合期接入，不由 AppKernel 偷偷创建或重复写日志。

OpenTelemetry Adapter 等待真实远程诊断需求。SQLite Diagnostics Metadata、崩溃恢复与跨进程历史属于 Phase 8，不能在本阶段绕过 PersistenceService 直接写库。

## 当前错误契约

| 范围 | 代表错误码 |
|---|---|
| Trace | `Trace.InvalidSpanName`、`Trace.SpanIdAlreadyExists`、`Trace.SpanAbandoned`、`Trace.ExporterThrew` |
| Metrics | `Metrics.InvalidValue`、`Metrics.KindConflict`、`Metrics.SeriesCapacityExceeded`、`Metrics.ExporterThrew` |
| Diagnostics | `Diagnostics.InvalidCheck`、`Diagnostics.AlreadyRegistered`、`Diagnostics.RegistryFrozen`、`Diagnostics.NotFound` |
| Log Exporter | `Observability.InvalidLogService`；底层 ILogService Error 原样交给本地服务的 exporter failure 缓存 |

## 已验收

- AppKernel 对三个本地服务的拥有、Ready 冻结和运行期快照入口；
- Command、Query、Task 的父子 Span、TraceId、终态和 completed/duration Metric；
- Task 成功、取消、截止、陈旧与执行 Span 状态的一致性；
- exporter 返回 Error 与抛异常不反转成功结果；观测包装自身异常不逃逸业务边界；
- LogObservabilityExporter 到真实 SpdlogLogService JSONL sink 的独立进程证明；
- Debug/Release 87/87、Debug 连续 20 轮、Production-only 和 46 个公共头/82 个生产源文件架构扫描。

## 阶段边界

本阶段只证明 Kernel 内存观测、诊断注册和 JSONL 导出契约。它不证明 SQLite Diagnostics Metadata、跨进程 Trace 汇聚、远程诊断、OpenTelemetry、告警系统、产品 Host、GUI、CAD/CAM、控制器 SDK 或物理设备能力。以上内容不得从自动化测试结果中推断为已实现或已验收。
