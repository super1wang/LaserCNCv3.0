# 2026-09-03 Phase 5 Unified Runtime 交付

## 结论

Phase 5 已验收。CommandRuntime、QueryRuntime、EventBus、Capability 与 AppKernel 生命周期形成统一内核执行入口，Headless/CLI 进程契约使用真实基础设施 Adapter 跑通。AppKernel 已关闭直接取得 TransactionManager 和运行期可变 DocumentStore 的逃生口。本阶段没有新增任何领域命令、上层模块或产品 Host。

## 交付内容

1. Descriptor 与发现
   - Command/Query Descriptor 携带稳定名称、Version、参数/结果 Schema、Capability 和执行元数据；
   - Registry 按稳定名称唯一注册并确定性枚举，AppKernel Ready 后冻结；
   - Phase 5 只允许同步 DocumentWrite Command，异步、Undo、文件和硬件副作用 fail-closed。
2. CommandRuntime
   - 固定执行 Registry、Schema、Idempotency、Capability、Expected ProjectRevision、ApplicationTransaction、Handler、结果 Schema、Commit、EventBus、Log；
   - Handler failure/exception 和结果 Schema failure 全部 rollback；
   - Project/Document 不匹配、旧 Revision、未授权 Session 均在写入前拒绝；
   - commit 后事件或日志失败写入 `postCommitErrors`，不反转已经提交的命令结果。
3. QueryRuntime
   - Query 只在 AppKernel Ready 状态运行；
   - Document 查询只接收不可变按值快照，并返回快照 Revision；
   - Capability、参数/结果 Schema、Project 所有权和日志均纳入统一链。
4. EventBus
   - 区分 Domain、Notification、System Event；Domain Event 只能来自成功 TransactionCommit；
   - 支持 Immediate/Queued、类别/名称过滤、RAII Subscription、Trace/Correlation；
   - Notification 可按 coalescing key 合并，Domain/System 不合并；
   - 回调在锁外执行，重入发布无死锁，异常被隔离并进入 delivery report。
5. Capability 与幂等
   - Session 未知或缺少精确 Capability 时默认拒绝；
   - 有限容量内存幂等表绑定业务签名，并发重复请求共享一次执行；
   - replay 返回原 TransactionCommit，不重复写状态或发布事件；
   - 幂等持久化和进程重启恢复明确留给 Phase 8。
6. AppKernel 边界
   - 组合阶段注入 ISchemaValidator/ILogService，注册命令与查询，装载空文档；
   - Ready 时冻结组合并开启 Runtime，shutdown 前检查活动事务/执行；
   - Host 只获得 const DocumentStore，不获得 TransactionManager。

## Headless/CLI 进程契约

`lasercnc_kernel_headless_contract --mode roundtrip` 是只在测试构建生成的独立进程。它注册测试专用 handler，使用 jsoncons 完成 JSON 和双向 Schema，执行带 ExpectedRevision/IdempotencyKey 的 Command，经 EventBus 后用 Query 读回，再由 spdlog 写入 JSONL。

该程序是内核协议的进程级 CLI 形态验证，不是产品 CLI。测试命令不会进入 production-only 构建，也不会成为未来 CAD/CAM 绕过领域不变量的通用对象写接口。

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
```

结果：

- Debug：72/72 CTest 通过；
- Release：72/72 CTest 通过；
- Debug repeat：72 项测试连续 20 轮通过，共 1440 次测试执行，包含 20 次独立 Headless/CLI 进程 roundtrip；
- Production-only Release：Foundation、Kernel、State、Runtime、Infrastructure 及生产依赖构建通过；未生成 Catch2、测试 target 或 Headless contract target；
- `git diff --check` 通过；
- 架构门禁检查 37 个 Kernel 公共头文件和 64 个生产源文件，未发现第三方或上层类型越界。

## 覆盖重点

- 注册冲突、Freeze、未配置 Runtime、启动前/关闭后拒绝；
- 参数/结果 Schema、Capability、Project 所有权、ExpectedRevision；
- Handler failure/exception、事务 rollback、post-commit 失败不反转；
- 同步与并发幂等 replay、key 重绑定；
- Query 不可变快照和活动执行关闭保护；
- Event 过滤、即时/队列、通知合并、订阅生命周期、重入和异常隔离；
- 真实 jsoncons/spdlog 的进程级发现、Command、Event、Query、JSONL 闭环。

## 未实现与后续

本交付不证明异步任务调度、进度/取消、资源仲裁、持久化幂等、Event Journal、崩溃恢复、产品 CLI/RPC/GUI，也不证明任何 CAD/CAM、控制器 SDK 或物理设备能力。

下一阶段严格进入 Phase 6：TaskRuntime、Scheduler、Cancellation、Resource Model。Queued Event 在 Phase 6 前保持显式 drain，不私建线程；异步 Command 在 TaskRuntime 建立前继续拒绝注册。
