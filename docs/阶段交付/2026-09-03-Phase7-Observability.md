# 2026-09-03 Phase 7 Tracing、Metrics 与 Diagnostics 交付

## 结论

Phase 7 已验收。Kernel 已形成自有的本地 Trace、Metric 与 Diagnostics 契约，AppKernel 统一拥有并冻结组合，Command、Query 和实际执行的 Task 进入同一 TraceId 链。观测记录、exporter/check 返回失败、回调抛异常以及运行时观测包装异常均不能改变业务控制流。本阶段没有新增 CAD、CAM、Machine、Process、Qt GUI、产品 CLI/RPC、AI 或其他上层模块。

## 交付内容

1. Trace
   - TraceId、SpanId 为不同 StrongId，支持可选 parentSpanId；
   - Span 固定 Running、Succeeded、Failed、Cancelled、Stale 状态；
   - 未显式结束的句柄自动记录 `Trace.SpanAbandoned`；
   - LocalTraceService 提供有界完成记录、活动数量和 exporter failure 快照；
   - exporter 在内部锁外调用，允许安全重入快照。
2. Metrics
   - 支持 Counter、Gauge、Histogram；
   - MetricName 与稳定排序 labels 构成 series 身份，同一身份不得改变类型；
   - 非有限值、负 Counter 和 series 容量耗尽均返回明确 Error；
   - 默认容量为 1024 个 series，运行时指标只使用有界 outcome 标签，不放入 RequestId/TaskId 等高基数身份。
3. Diagnostics
   - DiagnosticId 唯一注册，AppKernel Ready 时冻结；
   - runAll 按 ID 稳定排序，并保留每项最近报告；
   - check 返回失败、抛异常或返回错误 ID 均转为 Unhealthy 报告；
   - check 在注册表锁外执行，诊断不会自行恢复或改变内核状态。
4. 统一执行链接入
   - CommandRuntime、QueryRuntime 和 Scheduler 生成 `command.execute`、`query.execute`、`task.execute` Span；
   - 请求 parentSpanId 可接入未来 Workflow/Host 的父 Span；
   - 异步 Command 生成的 Task 自动继承 TraceId，并以 Command Span 为父节点；
   - Task Succeeded、Failed、Cancelled、Stale 映射到相同 Trace 终态；调度前终止不伪造执行 Span；
   - 三类执行记录 `kernel.<kind>.completed` 与 `kernel.<kind>.duration_ms`。
5. JSONL 导出
   - LogObservabilityExporter 同时实现 ITraceExporter 与 IMetricsExporter；
   - 只依赖 Kernel 的 ILogService，不暴露 spdlog 类型；
   - Span 和 Metric 分别写为结构化 `trace.span`、`metric.observation` LogRecord；
   - 使用 Phase 3 的 SpdlogLogService JSONL sink 获得逐行机器可读输出。
6. 生命周期与失败隔离
   - AppKernel 拥有 LocalTraceService、LocalMetricsService、DiagnosticsService；
   - exporter/check 只能在组合期注册，Ready 后冻结；
   - exporter Error/exception 进入有界 failure 快照；
   - 运行时对 span 创建和 metric 记录增加最外层异常隔离，不能在业务成功后反转返回，也不能阻断 Scheduler 的 notify/pump。

## Headless 进程契约

两个测试专用独立进程继续使用真实 JsonconsAdapter、SpdlogLogService 与 BS::thread_pool，并新增观测检查：

```text
同步链：JSON -> Command -> Event -> Query
                    \-> command/query Span + Metric -> ILogService -> JSONL

异步链：JSON -> Command -> TaskRuntime -> Scheduler -> Task
                    \-> Command Span -> child Task Span
                    \-> completed/duration Metric -> ILogService -> JSONL
```

进程测试会读取实际 JSONL 文件并确认同时存在 `trace.span` 与 `metric.observation`，同时校验异步 Command/Task 的父子 SpanId 和相同 TraceId。它们仍只是测试目标，不是产品 CLI。

## 自动化证据

环境：Windows、Visual Studio 2022、x64、MSVC 19.44.35216、Windows SDK 10.0.26100.0。

```powershell
cmake --build --preset vs2022-debug --parallel 12
ctest --preset vs2022-debug --output-on-failure

cmake --build --preset vs2022-release --parallel 12
ctest --preset vs2022-release --output-on-failure

ctest --preset vs2022-debug --repeat until-fail:20 --output-on-failure

cmake -S . -B build/production-only -G "Visual Studio 17 2022" -A x64 `
  -DLCNC_BUILD_TESTING=OFF -DLCNC_WARNINGS_AS_ERRORS=ON
cmake --build build/production-only --config Release --parallel 12

cmake -DLCNC_SOURCE_ROOT=J:/Code/LaserCNCv3.0 `
  -P cmake/VerifyKernelBoundaries.cmake
```

结果：

- Debug：87/87 CTest 通过；
- Release：87/87 CTest 通过；
- Debug repeat：87 项连续 20 轮通过，共 1740 次测试执行；
- Production-only Release：Foundation、Kernel、State、Runtime、Infrastructure 和生产依赖构建通过；`catch2SourcePresent=False`，测试/contract target 数量为 0；
- `git diff --check` 通过；
- 架构扫描检查 46 个公共头文件和 82 个生产源文件，未发现第三方类型或上层模块越界。

## 覆盖重点

- Span 重复、遗弃、父子关系、状态、内存容量与 exporter 重入；
- Counter/Gauge/Histogram 聚合、稳定顺序、类型冲突、非法数值和 series 容量；
- Diagnostics 唯一注册、冻结、稳定执行、latest、Error/exception/错误 ID 转换；
- Command/Query 成功和失败 Span、TraceId/parentSpanId、低基数 outcome Metric；
- 异步 Command 到 Task 的父子链、幂等 replay 不重复提交 Task；
- Task 成功、取消、截止、陈旧状态；
- exporter 返回 Error 和抛异常时业务结果保持不变；
- 真实 spdlog JSONL 的 `trace.span` 与 `metric.observation` 输出。

## 未实现与后续

本交付不证明 OpenTelemetry、远程 Trace Collector、告警平台、SQLite Diagnostics Metadata、跨进程历史、Snapshot、Journal、Crash Recovery、Workflow/Script、真实 CAD/CAM 任务、控制器 SDK 或物理设备能力。SQLite 元数据必须在 Phase 8 通过统一 PersistenceService 和应用持久化事务接入，不能由观测服务直接写库。下一阶段严格进入 Phase 8：Snapshot、Journal、SQLite Persistence 与 Crash Recovery。
