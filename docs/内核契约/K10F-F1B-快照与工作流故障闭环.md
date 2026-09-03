# K10F F1B 快照与工作流故障闭环

## 范围

本节点承接 F1A，补充 Snapshot、Hash、关闭元数据、执行后 Workflow checkpoint 与 Executor 接受故障，并修复 Executor submit 异常导致 Task 无法收尾的问题。只有内核端口语义、调度器、测试与中文文档改变，没有上层模块或新第三方依赖。

## 新增 54 个明确场景

| 路径 | 场景 | 核心断言 |
| --- | --- | --- |
| Snapshot | 摘要、begin、Journal 查询、文件写前/写后、索引 INSERT、commit、复用已有文件时的 read/hash，9 点 × 错误/异常 = 18 | 原索引保留；失败候选不成为可恢复快照；只有文件已发布阶段允许孤立文件；重启恢复原材料，重试同一 SnapshotId 可复用孤立文件 |
| 文档 close | Snapshot 索引失败、Detached 目录提交失败，2 点 × 2 = 4 | 内存文档保留，生命周期 Failed；已提交对象/Revision/Journal 不变；重启不自动加载为 Open，恢复材料仍可校验 |
| Journal / 幂等回执 Hash | Journal 初次摘要、写入后的验证摘要、command outcome 摘要，3 点 × 2 = 6 | 无部分 Document/History/Journal/持久 claim，重启原 Key 可以提交一次并重放 |
| Workflow 执行后 checkpoint | instance upsert、step delete/insert、commit、instance/step hash，6 点 × 2 × 本进程续行/重启续行 = 24 | Command 已执行且只提交一次；持久步骤仍为 Running attempt=1；同进程先补 checkpoint，重启复用同一幂等 attempt，均不重跑 handler 或追加 Journal |
| Executor 接受 | submit 返回错误/抛异常，2 | 已持久接受的 Task 最终 Failed；handler 未运行；后续任务取得相同独占资源和调度容量；shutdown 成功，重启保留失败任务而不重执行 |

共 5 个新增 Catch2/CTest 用例；与 F1A 的 46 个场景合并为 8 个故障矩阵用例、100 个明确场景。每个注入都检查实际命中，阶段依赖错误会导致测试失败，不允许只看 execute 返回失败。

Hash 注入根据序列化格式识别目标材料；Workflow 根 checkpoint 包含嵌套 step checkpoint，因此独立 step 摘要注入明确排除根格式，防止故障误落到执行前。

## Snapshot 与生命周期的可解释中间状态

Snapshot 的文件先原子发布，SQLite 索引后提交。文件写完但索引失败时，文件只是孤立材料，不会覆盖既有索引或被恢复入口自行采用。重试必须验证同一身份对应相同内容，不能静默覆盖。

close 的 Detached 目录提交失败时，数据库原始目录可能仍为 Closing；公开 `documentCatalog()` 按既有规则把 Opening/Closing 归一为 Failed。测试同时检查原始行和公开状态，不能把归一结果误认为底层已提交 Detached。重启后的 Failed 文档不自动打开；失败诊断与恢复材料保留，不增加自动“修复”入口。

## Workflow checkpoint 失败不撤销已提交 Command

执行后的 checkpoint 失败不是 Command 失败：Document/Revision/Journal 和 Command 幂等结果已经提交。Workflow 内存步骤为 Succeeded，持久步骤可能仍为 Running。该差异由 checkpointDirty 与原 attempt 的幂等恢复规则解释，不得通过重新执行 handler 来掩盖。

同实例再次 advance 先保存待落盘材料；新实例恢复 Running 为 Waiting，保留 attempt，CommandRuntime 返回原结果而不追加事务。正常销毁/重建实例的测试属于故障返回路径证据，不代替 F2 的真正子进程中断。

## Executor 异常修复

原 Scheduler 处理 submit 返回失败，但没有捕获 submit 抛出的异常。异常穿过 activate/CommandRuntime 后，已计入 runningCount、已持有资源的 Task 没有进入 finish，造成已持久接受任务与调用失败结果不一致，并可能阻塞后续任务或 shutdown。

现在 submit 异常转换为 `Task.ExecutorSubmitFailed`，复用既有 finish 路径释放资源、递减运行计数、记录 Failed、完成追踪并持久化终态。Command 接受回执仍表示已接受，不伪装 Task 已成功；通过 task 查询获得终态。

`ITaskExecutor` 的边界明确为：返回失败或抛异常时不得保留/调度 work 或 completion；成功必须恰好完成一次，允许同步 completion。内核不能安全撤销违反该约定、在报失败后仍暗中执行工作的适配器；此类实现不具备准入资格。

## 门禁

- Debug/Release 警告错误构建通过；Debug 全集 209/209（22.51 秒），Release 全集 209/209（17.00 秒）。
- F1A/F1B 的 8 个矩阵用例各重复 20 次，共 160 次 CTest 执行全部通过（75.52 秒）；每轮覆盖 100 个明确场景。
- Production-only Release 通过；31 个工程中测试/contract/Catch2 目标为 0，CTest 文件为 0，Catch2 源码目录不存在。
- 架构扫描通过 69 个公共头文件和 133 个生产源文件；Git 差异和新增文档链接检查通过。
- Executor 抛异常回归用例先确认失败，再修复并复验；返回错误分支及后续容量/资源使用一并通过。

日志位于忽略的 `build/k10f-f1b-debug-tests.log`、`build/k10f-f1b-release-tests.log` 与 `build/k10f-f1-repeat20.log`。完整 F1 准入见 [F1 故障注入验收](K10F-F1-故障注入验收.md)，F2–F5 仍需继续，Kernel 尚未 Frozen。本节点本地提交，K10F 大节点完成后再推送远端。
