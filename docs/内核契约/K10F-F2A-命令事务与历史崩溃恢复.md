# K10F F2A 命令、事务与历史崩溃恢复

## 范围与结论

本节点为 F2 的第一批独立进程证据，覆盖 Command、Transaction、History 共 10 个确定退出点。只扩展现有无界面契约测试与 CMake 测试编排，不修改生产 API、不添加测试后门、不增加第三方库或任何上层模块。

F2A 不代表完整 F2 验收；实际 Task/Workflow handler 中断、Asset 发布中断、External Effect 外部调用计数仍需继续。Kernel 1.0 尚未 Frozen。

## 三进程验证链

每个场景依次启动三个独立进程，复用同一个专用目录中的真实 SQLite 数据库与文件快照：

1. `crash-seed` 建立基线对象与快照，然后在实际 Command handler 或 SQLite 委托调用边界执行 `std::_Exit(86)`，不调用 shutdown、不展开析构、不主动 rollback。
2. `crash-recover` 新建 Kernel 并恢复，先核对崩溃后状态，再执行显式重试或 Undo/Redo，最后正常 shutdown。
3. `crash-audit` 再次新建 Kernel，确认恢复进程的最终状态可再次恢复，并且没有调用测试 handler。

编排器必须同时收到退出码 86 和对应场景的退出点标记。正常返回、异常、超时或错位标记均失败；每个子进程限时 30 秒，单条 CTest 限时 100 秒。

每轮在 `build/vs2022/tests/crash-contract-runs/<场景>-<随机后缀>` 创建独立目录。编排器不递归删除调用者传入的路径；数据库、快照和三个进程的 stdout/stderr/退出码日志均保留在忽略的构建产物中。标记由子进程输出、父进程保存，不将它解释为断电耐久性证明。

## 10 个退出点

| 场景 | 实际退出位置 | 首次恢复状态 |
| --- | --- | --- |
| `command-staged` | handler 已形成对象/Geometry Revision 候选，尚未返回 | 仅基线对象，Revision=1，History=1/1，claim abandoned |
| `journal-inserted` | SQLite 已执行 Journal INSERT，事务未提交 | 同上，未提交 Journal 不可见 |
| `outcome-written` | SQLite 已更新 completed 回执，但所属事务未提交 | 同上，不能把未提交回执认作成功 |
| `transaction-before-commit` | 全部事务材料写入、调用 SQLite commit 前 | 同上，整体丢弃未提交事务 |
| `transaction-committed` | SQLite commit 成功返回、内核内存尚未安装 | 目标对象存在，Revision=2，History=2/2，claim completed |
| `command-returned` | executeCommand 成功返回后、shutdown 前 | 同上，回执可重放 |
| `undo-inserted` | Undo Journal INSERT 后、commit 前 | 目标对象仍存在，Revision=2，History=2/2 |
| `undo-committed` | Undo commit 成功、内存尚未安装 | 目标对象已删除，Revision=3，History=1/2，Redo 材料保留 |
| `redo-inserted` | 已有一次 Undo；Redo Journal INSERT 后、commit 前 | 目标对象仍删除，Revision=3，History=1/2 |
| `redo-committed` | Redo commit 成功、内存尚未安装 | 目标对象恢复，Revision=4，History=2/2 |

表中的 Revision 同时指 Project、Document、Geometry 三个作用域；Cam、MachineContext、Environment 始终为 0。History 使用 `position/extent` 表示游标。

除 `command-returned` 外，退出点内还检查活动 Document 的对象与 Revision 与执行前快照完全一致，证明没有提前安装候选；SQLite commit 成功后的这个窗口允许持久状态领先内存，启动恢复必须补齐。

## 一致性断言

- 所有恢复检查 Project ownership、对象数量/身份/类型/SchemaVersion/Value、空资产引用、六类 Revision、History cursor/entry 数量/barrier 和 Journal 逐条 Revision 链。
- 基线 Snapshot 与之后的 Journal 共同参与恢复，不依靠重新执行 handler 重建对象。
- 未提交的命令 claim 在 bootstrap 后为 abandoned；显式提交原 Request/Key 才执行一次 handler，之后重放。
- 已提交的命令 claim 为 completed；原 Request/Key 返回缓存结果与原解析版本，不再次执行 handler、不增加 Journal。
- Undo/Redo 没有幂等 Key，恢复不会猜测并重复执行。测试按已恢复游标显式完成未提交操作，再执行相反方向，验证保留的 Undo/Redo 材料可用。
- History 操作后即使目标对象已被 Undo 删除，重放原创建命令仍只返回原回执，不重新创建对象。恢复进程与最终审计进程检查确定的对象/Revision/Journal/History 组合状态。
- 这里的对象资产列表为空；不能把该材料检查作为 Asset 崩溃认证。

## 复现与门禁

```powershell
cmake --build --preset vs2022-debug --parallel 1 -- /nodeReuse:false
ctest --preset vs2022-debug -R 'integration.kernel_crash_' --output-on-failure
ctest --preset vs2022-debug -R 'integration.kernel_crash_' --repeat until-fail:20 --output-on-failure
```

源证据：`tests/integration/kernel_crash_contract.cpp`、`cmake/RunKernelCrashContract.cmake`、`tests/CMakeLists.txt`。生产目标不编译本测试文件；`lasercnc_kernel_headless_contract` 仍是测试程序，不是产品 CLI。

本节点门禁结果：

- Debug/Release 警告错误构建通过；最终 Debug 全集 219/219（26.81 秒），Release 全集 219/219（26.80 秒）。
- 10 个崩溃场景各连续重复 20 次，共 200 次 CTest 执行、600 个子进程全部通过（220.12 秒），未出现 flaky、超时或恢复状态分裂。
- Production-only Release 通过；31 个工程中测试/contract/Catch2 目标为 0，CTest 文件为 0，Catch2 源码目录不存在。
- 架构扫描通过 69 个公共头文件和 133 个生产源文件；全量 CTest 同时覆盖既有架构负例。
- Git 差异检查、中文文档链接检查通过，抽查退出/恢复/审计三个进程日志与数据库文件保留正常。

日志为 `build/k10f-f2a-debug-tests.log`、`build/k10f-f2a-release-tests.log`、`build/k10f-f2a-repeat-tests.log` 和对应的 `*-build.log`。本节点只做本地 Git 提交；完整 K10F 大节点通过后再推送远端。

## 明确边界

进程终止覆盖指定的接口/事务窗口，不穷举 SQLite 内部机器指令、OS 缓存、文件系统或存储硬件掉电行为；不认证控制器、运动、激光与物理机器安全。Task、Workflow、Asset、External Effect 的真实执行中断为后续 F2 工作，F3/F4/F5 也尚未闭合。
