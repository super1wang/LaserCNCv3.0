# K10F F3B：取消竞态与生命周期压力

## 范围

承接 F3A，只完善内核可靠性，不增加上层模块或生产公共 API。新增压力测试复用现有 Catch2/CTest、ModuleRegistrar、ExecutionGateway、真实 BS 线程池、SQLite 和文件快照。运行记录与汇总门禁尚在收集中，本文件不提前宣称完整 F3 或 Kernel Frozen。

## 实测发现与修复

新增“运行中重复取消”测试在首轮直接发现生产缺陷：Task handler 仍被同步门持有，8 个取消调用后状态已变为 `Cancelled`，而预期应保持 `CancelRequested`。

原因位于 `Scheduler::requestCancel`：此前仅将 `Running` 视为运行态，已经 `CancelRequested` 的任务在再次取消时进入“执行前取消”分支，提前持久化终态。`finish` 只处理 Running/CancelRequested，因此真实完成回调可能不再释放 runningCount 和独占资源；Document close 的活动任务检查也可能漏掉尚未返回的 handler。

修复仅将 Running 与 CancelRequested 共同归入运行中取消分支。实际完成回调继续作为资源释放、容量回收和终态持久化的唯一入口。不强制终止线程，不把取消请求等同于执行结束，不改变已完成任务的终态。

失败证据保留在 `build/k10f-f3b-repeated-cancel-before-fix.log` 及其中记录的独立数据库目录。修复后的同一测试要求 handler 放行前仍持有资源、close 仍拒绝；放行后实际运行另一个使用相同独占资源的 Task，证明容量与资源确实可复用。

## 固定矩阵

| 场景 | 轮次与交错 | 关键检查 |
| --- | --- | --- |
| Task 完成前取消 | 20 轮；真实 worker 进入 handler 后，8 路并发 cancel，再放行 | 始终保持 CancelRequested；close 返回 CloseBlocked；资源/活动计数保留；完成后 Cancelled，无成功结果 |
| Task 完成后取消 | 20 轮；waitTask 已观察成功后，8 路并发 cancel | 成功结果及终态不变，不重复执行 |
| Task 取消/完成竞争 | 20 轮；8 路 cancel 与 1 个 handler 放行者从同一同步门出发 | 允许 Succeeded 或 Cancelled，结果/错误与终态匹配；资源无残留、可运行后续 Task |
| Task shutdown 超时 | 20 轮；真实 handler 保持运行，shutdown(1ms) 确定超时，追加 8 次 cancel | AppKernel 保持 Stopping，拒绝新 Command/Query/close；资源仍占用；实际完成后再次 shutdown 成功 |
| Workflow 运行中取消 | 20 轮；两步 DocumentWrite，第一个已进入 handler 时 8 路 cancel | 第一笔已接受 Command 正常完成并保留 History，第二步不执行；根状态 Cancelled，第一步 Succeeded、第二步 Cancelled/attempt=0 |
| Workflow 完成后取消 | 20 轮；两个步骤完成后 8 路 cancel | Succeeded、两笔提交、两个 attempt=1 不变 |
| Workflow 取消/完成竞争 | 20 轮；8 路 cancel 与第一步 handler 放行者同步竞争 | 允许 Cancelled/Succeeded；保留实际完成的一或两笔提交，未开始步骤不伪造 attempt；终态再次 advance 不执行 |
| Module 失败回滚 | 20 轮 × 3 种：全部贡献注册后返回错误、抛异常、后继模块 start 失败 | Service、Command、Query、Task、Workflow、Script、ObjectType 注册移除；weak_ptr 证明服务对象已释放；新组合可用相同身份正常启动/停止 |

Task 与 Workflow 每轮使用独立持久化目录；在新 AppKernel 中检查恢复的终态、回执和文档状态，不复活执行。Task 还核对接受回执重放不重跑 handler；Workflow 核对恢复后 cancel/advance 不更改终态和步骤 attempt。写入相关场景同时核对六类 Revision、对象材料、Journal 链、History cursor；所有终态检查零活动准入并实际 shutdown。

Module 的健康对照配置测试专用 Inline executor，仅证明组合可重新建立，不作为真实 worker 竞态证据。真实 worker 证据来自 Task 矩阵。模块首版健康对照曾遗漏必需的 executor，已补齐；生产准入规则未放宽。

## 有界性与证据

同步门/future/活动观察沿用 F3A 的 5 秒期限与 RAII 释放顺序，不靠随机 sleep。真实 SQLite 多轮 checkpoint、close/open/restart 的累计耗时与单个同步点超时不同；基础设施 CTest 外层上限从 120 秒调整为 300 秒，内核 CTest 增加 120 秒上限。外层上限只保护整例挂起，不替代内部同步期限。

逐轮数据仍保留在 `build/vs2022/tests/stress-contract-runs/`，不递归清理传入路径。测试专用 gate、handler 和快照装饰器不进入生产目标。

## 本地检查点证据

- 最终 Debug 全集 250/250 通过，443.37 秒；包含新增八个 F3B 用例、既有 F3A、故障注入与独立进程恢复。
- Debug/Release 自有代码均已在 `/W4 /WX /permissive-` 下构建通过。
- 纯生产 Release 构建通过；31 个工程，测试/contract/Catch2 工程 0，CTest 文件 0。架构扫描通过 69 个公共头文件、133 个生产源文件。
- 七个 Task/Workflow 专项在开发过程中均已通过；模块健康对照修正后独立运行通过。开发期综合日志中的旧健康对照失败不计为最终通过证据，以最终 Debug 全集为准。
- Release 全集仍在运行；最终版本完整 F3 重复矩阵与汇总结论待补齐。本次只提交本地修复检查点，不推送远端认证节点。

日志：`build/k10f-f3b-debug-tests.log`、`release-tests.log`、`debug-build.log`、`release-build.log`、`production-build.log`（后三种及 Release 日志文件名均带相同 `k10f-f3b-` 前缀）；开发期专项为 `k10f-f3b-task-focused.log`、`k10f-f3b-focused.log`、`k10f-f3b-module-focused.log`。数据文件和本机日志不进入 Git。

F4 性能/内存及 F5 ASan/最终审计仍在后续，不能把本轮小对象压力当作 Benchmark 或物理设备准入。
