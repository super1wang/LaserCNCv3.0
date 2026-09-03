# Phase 7 本地观测与诊断契约

## 阶段目标

Phase 7 建立 Kernel 自有的 Trace、Metrics 与 Diagnostics 语义，并确保观测失败不改变 Command、Query、Task 或 Transaction 的业务控制流。第一节点仅实现线程安全本地服务和 exporter/check 端口；关键执行链接入、多配置和独立进程验收尚未完成，因此本阶段保持“进行中”。

当前不引入 OpenTelemetry SDK，不把 SQLite 当作日志或 Trace backend，也不实现远程诊断、MES、数字孪生、Cloud 或任何上层模块。

## Trace

- ITraceService 创建 ITraceSpan；TraceId 表示完整链路，SpanId 表示单个执行区间，可选 parentSpanId 建立父子关系。
- Span 状态为 Running、Succeeded、Failed、Cancelled、Stale；同一 SpanId 在活动和保留窗口内不可重用。
- Span 必须显式 end；句柄销毁前未完成时自动记录 `Trace.SpanAbandoned`，不能静默丢失。
- LocalTraceService 先在锁内完成本地记录，再复制 exporter 快照并在锁外调用；exporter 返回失败或抛异常只进入有界 exporterFailures，不反转 Span 或业务结果。
- 完成记录采用有界 FIFO 保留。容量至少为 1，默认记录 4096 条、exporter 失败 256 条。

## Metrics

- 支持 Counter、Gauge、Histogram；MetricName 与有序 labels 共同构成 series 身份。
- 同一 series 不得切换 MetricKind；所有观测必须为有限数值，Counter delta 不得为负。
- Counter 保存累计值，Gauge 保存最新值，Histogram 保存 count/sum/min/max；快照按稳定 MetricName/labels 顺序输出。
- exporter 在本地聚合完成后于锁外调用；失败仅进入有界 exporterFailures，记录方法仍成功。
- series 数量有界，默认 1024；达到容量后新 series 返回 `Metrics.SeriesCapacityExceeded`。运行时接入必须忽略该观测错误，不得影响业务结果。
- 默认 labels 禁止放入 TaskId、RequestId、ObjectId 等无界高基数身份；这些身份属于 Trace/Log 上下文。

## Diagnostics

- DiagnosticId 是稳定 StrongId；IDiagnosticCheck 只在组合期唯一注册，Ready 边界冻结。
- run 与 runAll 在注册表锁外执行 check，check 可以安全读取 latest 快照。
- check 返回 Error、抛异常或返回错误 ID 时转换为当前 DiagnosticId 的 Unhealthy 报告，而不是让诊断调用崩溃。
- runAll 按 DiagnosticId 确定性执行；latest 只保存每项最近报告。
- Diagnostics 报告状态为 Healthy、Degraded、Unhealthy、Unknown，不等同于 AppKernel 生命周期状态，也不能自行执行恢复动作。

## Exporter 与持久化边界

ITraceExporter 与 IMetricsExporter 是稳定扩展端口。Local 服务始终先保留有界内存事实，再通知 exporter。Phase 7 后续可提供基于统一 ILogService 的 JSONL exporter；OpenTelemetry Adapter 等待真实远程诊断需求。SQLite Diagnostics Metadata、崩溃恢复与跨进程历史属于 Phase 8，不能在本阶段绕过 PersistenceService 直接写库。

## 当前错误契约

| 范围 | 代表错误码 |
|---|---|
| Trace | `Trace.InvalidSpanName`、`Trace.SpanIdAlreadyExists`、`Trace.SpanAbandoned`、`Trace.ExporterThrew` |
| Metrics | `Metrics.InvalidValue`、`Metrics.KindConflict`、`Metrics.SeriesCapacityExceeded`、`Metrics.ExporterThrew` |
| Diagnostics | `Diagnostics.InvalidCheck`、`Diagnostics.AlreadyRegistered`、`Diagnostics.RegistryFrozen`、`Diagnostics.NotFound` |

## 尚未验收

- AppKernel 对三个本地服务的拥有、冻结和公共只读入口；
- Command、Query、Task 的 Span 层级、终态和耗时 Metric；
- Task 进度/取消/陈旧与 Trace 状态一致性；
- 基于 ILogService 的 JSONL exporter 与真实 spdlog 进程证明；
- Debug/Release、重复、Production-only 与架构门禁。
