# K10F F3A：命令查询与生命周期并发压力

## 范围与状态

本节点已通过下述门禁，只补充 Application Kernel 的并发可靠性证据，不扩展领域模块、Host 或生产公共 API。F1/F2 的历史验收保持不变；F3A 不等于完整 F3，更不等于 Kernel 1.0 Frozen。

测试入口：`tests/unit/runtime/kernel_concurrency_tests.cpp`，由现有 `lasercnc_infrastructure_tests` 承载，通过 ModuleRegistrar 配置测试贡献，通过 ExecutionGateway 执行 Command/Query。使用实际 Jsoncons、SQLite、SHA-256 与 Windows 文件快照，不使用内存数据库替代持久化。

## 固定交错矩阵

每个用例固定 20 轮，每轮独立内核、数据库及快照目录；工作线程不调用 Catch2 断言，主线程汇总所有结果。

| 场景 | 明确交错 | 每轮关键断言 |
| --- | --- | --- |
| 16 路 Query | 全部进入 handler 并持有文档 Query 租约后，调用 close/shutdown | close 返回 `Document.ActiveOperations`、shutdown 返回 `Kernel.ActiveExecutions`；16 份返回值和六类 Revision 一致；无 Journal/History 增量；结束后 close/open/shutdown 正常 |
| 16 路同键 Command | 唯一 handler 创建候选并暂停，等到 16 个请求都持有 Command 租约后放行 | handler 恰好 1 次；15 份 replay、1 份原始回执；TransactionId/结果一致；仅 1 个对象、1 条 Journal/History、一次 Revision 增量；新内核重放同键不执行 handler |
| 8 路 Revision 竞争 | 8 个独立请求各自基于 Revision 0 建立候选，全部到达提交前同步门后一起释放 | 1 个成功、7 个 `Project.RevisionConflict`；失败候选不进入当前状态/历史；重启后仍只有同一个胜者 |
| Closing 排斥准入 | 实际 close 已进入 Closing，在文件快照写入口暂停，同时发起 16 个 Query | 全部返回 `Document.NotOpen`，Query handler 次数为 0；close 完成 Detached 后 open 成功，新的 Query 可正常运行 |

所有场景结束后核对六类 Document activity 均为 0、Scheduler 无活动 Task，且 shutdown 成功。写场景核对 Project/Document/Geometry 各增加一次，Cam/MachineContext/Environment 不变，同时验证对象类型、数据、资产集合、History cursor 与 Journal Revision 链。

Transaction 的 Document activity 仅用于 `begin` 准入，不代表整个事务生存期。首轮测试曾误将该值视为活动事务数，产生两个测试断言失败；已依据实现修正。候选运行中通过 shutdown 的 `Kernel.ActiveTransactions` 拒绝结果验证事务表阻塞，结束后 shutdown 成功验证清空。未因测试假设而修改生产契约。

## 超时、异常收尾与证据保留

- 同步门到达/放行、future 观察及活动计数观察均有 5 秒期限；使用条件变量或有界 yield，不依赖随机 sleep 建立交错。
- 同步门的 RAII 释放对象在 future 之后声明，主线程断言/异常退出时先释放 handler，再析构 future；避免测试自身把线程永久留在门上。
- F3A 引入基础设施 CTest 的 120 秒外层超时；后续 F3B 因多轮持久化压力扩大为 300 秒，同步点仍为 5 秒。这是潜在生产死锁的最后防护，不替代测试辅助等待的期限，也不是证明所有生产阻塞操作已有超时。
- Closing 探针只是测试专用 `ISnapshotStore` 装饰器，继续委托真实文件存储；未在生产类中增加注入开关。
- 每轮证据保留于 `build/vs2022/tests/stress-contract-runs/<唯一目录>/`，包括 `state.db` 和快照文件。拒绝复用已存在的目录，不递归清理输入路径；失败时 Catch2 输出轮次和证据路径。

## 验证记录

当前代码的 Debug 四项专项已通过（53.92 秒）。随后使用 CTest `-j 4` 并行四个独立测试进程，三次连续重复全部通过（157.47 秒）：共 12 次测试执行、240 个独立轮次、3,360 次并发 Command/Query 请求，另含回执重放、reopen 查询和 close/shutdown 检查。该计数不包含首轮开发失败，也不把每轮多个断言算成独立场景。

Production-only Release 通过：31 个工程，测试/contract/Catch2 工程 0，CTest 文件 0。架构门禁仍为 69 个公共头文件、133 个生产源文件。Debug/Release 自有代码构建继续使用 `/W4 /WX /permissive-`。

Debug 全集 242/242 通过（121.07 秒），Release 全集 242/242 通过（120.66 秒），包含既有 F1 故障矩阵、F2 独立进程恢复和架构负例。最终代码重复运行中未出现 flaky、deadlock 或 partial state；该结论限于本记录的配置、轮次与交错。

本节点作为 F3A 本地 Git 检查点提交；F3B 完成并汇总完整 F3 证据后再同步远端重要节点。

复现命令（仓库根目录）：

```powershell
cmake --build --preset vs2022-debug --parallel 1 -- /nodeReuse:false
ctest --preset vs2022-debug -R 'Kernel concurrency' -j 4 --repeat until-fail:3 --output-on-failure
ctest --preset vs2022-debug --output-on-failure
cmake --build --preset vs2022-release --parallel 1 -- /nodeReuse:false
ctest --preset vs2022-release --output-on-failure
cmake --build build/production-only --config Release --parallel 1 -- /nodeReuse:false
cmake -DLCNC_SOURCE_ROOT=J:/Code/LaserCNCv3.0 -P cmake/VerifyKernelBoundaries.cmake
```

当前本机日志位于忽略目录 `build/k10f-f3a-*`：构建日志分别为 `debug-build.log`、`release-build.log`、`production-build.log`，专项日志为 `debug-focused.log`、`repeat3.log`，全集日志为 `debug-tests.log`、`release-tests.log`。这些日志和逐轮二进制数据库不进入 Git；源代码、复现步骤和验收摘要进入 Git。

## 剩余边界

F3A 交付时尚未完成的 Task/Workflow 运行中取消与完成竞态、close vs Task、模块失败重复回滚，现已由 [F3B](K10F-F3B-取消竞态与生命周期压力.md) 补齐，见 [完整 F3 验收](K10F-F3-并发与生命周期验收.md)。本节点只证明上述明确交错，不宣称 AppKernel 配置/bootstrap/shutdown 可以由任意多个 Host 线程无约束并发调用，也不宣称覆盖所有调度顺序。

F4 性能/内存、F5 ASan 和最终门禁仍未完成。测试中的少量对象是并发一致性载荷，不是性能基线；软件测试不代表物理设备或机器安全准入。
