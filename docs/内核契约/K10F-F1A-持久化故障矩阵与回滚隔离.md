# K10F F1A 持久化故障矩阵与回滚隔离

## 节点范围

本节点扩展真实 SQLite 委托后端的故障注入，验证事务/History、幂等与 Task 接受的失败状态，并修复回滚失败后连接仍可用的问题。只涉及内核持久化、测试和中文文档，不增加上层模块、第三方库或生产测试开关。

F1A 是 F1 的内部节点，不等于整个 F1 或 Kernel 1.0 可靠性认证通过。Snapshot/Hash 分阶段矩阵、执行后 Workflow checkpoint 故障与 F2–F5 仍待完成。

## 新增矩阵

测试专用 `FaultInjectingBackend` 在委托真实 SQLite 前，对指定调用点/SQL 片段/第几次匹配注入一次返回错误或异常。每个场景必须断言注入实际命中，避免因请求在更早阶段被拒绝而形成假阳性。故障点位于真正数据库操作之前；不将这种注入称为“提交已落盘但结果丢失”。

| 路径 | 故障点 | 场景数与断言 |
| --- | --- | --- |
| 可撤销普通写入 | 幂等 claim begin、Journal begin、INSERT、readback、幂等 outcome UPDATE、commit | 6 点 × 错误/异常 = 12；Document/Revision/cursor/Journal 不变，无持久 claim，重启后原 Key 可以提交一次并重放 |
| Undo / Redo | Journal begin、INSERT、readback、commit | 每种操作 4 点 × 2，共 16；状态和 cursor 不变，重启重试只推进一次对应历史操作 |
| Task 接受 | accept begin、task_history INSERT、接受 outcome UPDATE、commit | 4 点 × 2 × 两条重试路径 = 16；prepare 已执行、Task handler 未执行，无持久任务/claim，准备态清除 |
| 回滚自身失败 | Journal commit 前失败，再令 rollback 返回错误或抛异常 | 2；连接永久隔离、Ready=false、不允许读 Journal/重新初始化/再次提交，新实例恢复后可正常提交 |

上述共 46 个明确场景，组织为 3 个 Catch2/CTest 用例。事务/History 场景比较完整对象材料、RevisionSet、History cursor 与 Journal 数量，不只检查失败返回值。

内建 Undo/Redo 目前不声明幂等 Key 支持；测试不擅自修改该契约。普通 Command 的同进程幂等缓存保留失败结果，所以 Task 测试分别验证：

1. 失败后使用原 Key 仍得到缓存错误且 prepare 不重跑；使用新 Key 和相同 TaskId 可以接受，证明 Scheduler 的 prepared record 已移除；重启重放成功结果不执行 handler。
2. 失败后直接重建 Kernel，原 Key 和相同 TaskId 可重新接受，证明数据库没有残留 completed/pending 绑定阻止恢复。

## 发现的问题与修复

原实现只返回 `Persistence.RollbackFailed`，没有撤销持久化 Ready 状态。SQLite rollback 返回失败时连接可能仍处于事务中，此时 Journal 查询可能读取本连接尚未提交的材料；rollback 抛异常时外层 catch 还可能重复尝试回滚，再继续使用此连接。

现由 PersistenceService 内部 `QuarantiningBackend` 统一包装注入后端。rollback 调用前暂时撤销 initialized；只有明确成功才恢复原状态。返回错误或异常均永久隔离该连接：

- `ready()` 变为 false；普通持久化操作按 NotReady 拒绝。
- 底层 execute/query/begin/commit/rollback 均不能再触及被隔离的委托；重复 initialize 返回 `Persistence.BackendQuarantined`。
- 回滚异常被转换为 `Persistence.RollbackException`，外层保留主失败 cause 和 `Persistence.RollbackFailed`，不会再做隐式重试。
- 保留最后已提交的内存 Document/History，不安装失败候选；既有内存快照和回执不被伪装成新的持久提交。
- 不进行自动重连或自动重放。销毁旧实例时由后端释放连接，随后使用新 Kernel/PersistenceService 执行正常恢复。SQLite 未提交事务不会在该重启路径中变成已提交日志。

隔离不等于立即关闭连接；在旧实例销毁前可能继续持有数据库锁。Host 应将持久化失去 Ready 视为需要停止新持久工作并重建恢复的故障，不能循环重试同一实例。该机制不是物理掉电或任意第三方驱动提交结果不明的证明；后者仍需要对应故障和独立进程证据。

## 验证

- VS2022 x64 Debug/Release 警告错误门禁构建通过。
- Debug 全集 204/204，18.30 秒；Release 全集 204/204，14.17 秒。
- 3 个新增故障矩阵用例各连续 20 次，共 60 次 CTest 执行全部通过，30.48 秒；每轮覆盖上述 46 个场景。
- Production-only Release 构建通过，31 个工程中测试/contract/Catch2 目标为 0，CTest 文件为 0。
- 架构扫描通过 69 个公共头文件与 133 个生产源文件；没有新增公共接口或上层依赖。
- 本次先运行回滚隔离回归测试确认失败，再实现修复并复验；Git 差异检查通过。

日志位于忽略的 `build/k10f-debug-tests.log`、`build/k10f-release-tests.log`、`build/k10f-f1a-repeat20.log`。本节点只作本地提交，K10F 大节点完成后再统一推送远端。
