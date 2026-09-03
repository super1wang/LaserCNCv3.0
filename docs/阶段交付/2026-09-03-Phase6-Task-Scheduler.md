# 2026-09-03 Phase 6 TaskRuntime 与 Scheduler 交付

## 结论

Phase 6 已验收。Kernel 已形成 `TaskRuntime -> Scheduler -> ITaskExecutor -> BsThreadPoolExecutor` 的长任务唯一执行栈，任务状态、依赖、取消、截止时间、进度、资源、文档快照与修订陈旧检测均由自研内核掌握。异步 Command 继续使用 Phase 5 的同一个 CommandRuntime 入口。本阶段没有新增 CAD、CAM、Machine、Process、Qt GUI、产品 CLI/RPC、AI 或其他上层模块。

## 交付内容

1. Task 契约与注册
   - TaskId、TaskName、ResourceId 为独立 StrongId；
   - TaskDescriptor 携带 Version、输入 Schema 和结果 Schema；
   - TaskRegistry 唯一注册、确定性发现，并随 AppKernel Ready 冻结；
   - 输入与结果均使用 ExecutionServices 中的统一 ISchemaValidator。
2. 状态、取消、进度与截止时间
   - 固定 Pending、Ready、Running、Succeeded、Failed、CancelRequested、Cancelled、Stale 八态；
   - CancellationToken 同时传播显式取消和 Deadline，不允许模块私建第二套 stop/quit 标志；
   - ProgressReporter 只接受 `[0, 1]` 有限值和单调前进；成功终态归一为 1.0；
   - 非协作任务不被强杀；有界关闭超时明确返回 `Task.ShutdownTimeout`。
3. Scheduler
   - 按依赖、优先级、同优先级 FIFO、Executor 并发度与资源可用性调度；
   - 依赖未完成保持 Pending，依赖失败/取消/陈旧使下游进入 Stale；
   - 高优先级任务资源受阻时继续寻找非冲突候选，避免无关任务饥饿；
   - Executor work、completion 与 handler 异常均被统一 Error 边界隔离。
4. Resource Model
   - 支持 CPU、DiskIO、GPU、OCCT、ProjectRead、ProjectWrite、MachineController、CollisionBackend；
   - 全部声明原子获取、对称释放，不持有部分资源等待；
   - Shared 支持容量/units，Exclusive 与所有持有者互斥；
   - ProjectRead/ProjectWrite 规范化到同一资源槽，ProjectWrite 强制 Exclusive。
5. 不可变文档与修订
   - 文档任务提交时校验 Project/Document 所有权和 ExpectedRevision；
   - handler 只获得按值不可变 Document 与六域 Revision 快照，不获得 DocumentStore 或 Transaction；
   - 完成时源修订已变化则返回 Stale；结果携带 sourceRevisions，未来应用仍须进入 Command/ApplicationTransaction 再检查修订。
6. 异步 Command
   - IAsyncCommandHandler 只生成只读 Task 计划和接受结果；
   - CommandRuntime 统一执行参数/结果 Schema、Capability、ExpectedRevision、Trace/Correlation 和内存幂等；
   - 接受结果返回 TaskId，不返回伪 TransactionCommit；相同幂等业务请求不重复提交任务；
   - DocumentWrite、文件发布、控制器或运动副作用不得进入异步 handler。
7. AppKernel 生命周期
   - 组合期注入并独占 ITaskExecutor，配置资源和注册任务；
   - 缺少 Executor 或 ExecutionServices 时 fail-closed；
   - 关闭先停止接收、协作取消、限时等待、关闭 Executor，再停止模块；
   - 超时时保持 Stopping 状态，任务退出后可以重试关闭，不能伪报 Stopped。

## Headless 进程契约

`lasercnc_kernel_headless_contract --mode task-roundtrip` 只在测试构建生成。它使用真实 JsonconsAdapter、SpdlogLogService 与 BsThreadPoolExecutor，执行以下闭环：

```text
JSON 参数
  -> 异步 CommandRuntime
  -> Capability / ExpectedRevision / Idempotency
  -> TaskRuntime / Scheduler / BS::thread_pool
  -> 不可变 Document + Trace/Correlation
  -> Progress + 结果 Schema
  -> JSON 结果 + JSONL 日志 + 有界关闭
```

该程序是内核协议的独立进程证明，不是产品 CLI，也不包含真实领域任务。

## 自动化证据

环境：Windows、Visual Studio 2022、x64、MSVC 19.44.35216、Windows SDK 10.0.26100.0。

```powershell
cmake --build --preset vs2022-debug --parallel 16
ctest --preset vs2022-debug --output-on-failure

cmake --build --preset vs2022-release --parallel 16
ctest --preset vs2022-release --output-on-failure

ctest --preset vs2022-debug --repeat until-fail:20 --output-on-failure

cmake -S . -B build/production-only -G "Visual Studio 17 2022" -A x64 `
  -DLCNC_BUILD_TESTING=OFF -DLCNC_WARNINGS_AS_ERRORS=ON
cmake --build build/production-only --config Release --parallel 16

cmake -DLCNC_SOURCE_ROOT=J:/Code/LaserCNCv3.0 `
  -P cmake/VerifyKernelBoundaries.cmake
```

结果：

- Debug：83/83 CTest 通过；
- Release：83/83 CTest 通过；
- Debug repeat：83 项连续 20 轮通过，共 1660 次测试执行，包含同步与异步两条独立进程闭环各 20 次；
- Production-only Release：Foundation、Kernel、State、Runtime、Infrastructure 和生产依赖构建通过；`catch2SourcePresent=False`，测试/contract target 数量为 0；
- `git diff --check` 通过；
- 架构扫描检查 42 个公共头文件和 74 个生产源文件，未发现第三方类型或上层模块越界。

## 覆盖重点

- Registry Freeze、错误 handler 类型、重复 TaskId、缺失/重复/自依赖；
- Pending、Ready、Running、Succeeded、Failed、CancelRequested、Cancelled、Stale；
- 显式取消、Deadline、进度范围/回退、handler failure/exception；
- 优先级、FIFO、依赖传播、资源读写互斥、非冲突任务防饥饿；
- 不可变 Document、ExpectedRevision、源修订变化后的 Stale；
- Scheduler 与 AppKernel 两层有界关闭超时和可重试关闭；
- 异步 Command 的 Capability、上下文覆盖、TaskId 返回和幂等 replay；
- 真实 jsoncons/spdlog/BS::thread_pool 的进程级闭环。

## 未实现与后续

本交付不证明 Trace backend、Metrics、Diagnostics、任务持久化、Event Journal、Snapshot/Crash Recovery、Workflow/Script、真实 CAD/CAM 长任务、控制器 SDK 或物理设备能力。Queued Event 保持显式 drain，Scheduler 不冒充 Host 事件循环。下一阶段严格进入 Phase 7：Tracing、Metrics、Diagnostics。
