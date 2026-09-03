# K10F F2C 资产发布中断与引用恢复

## 范围

本节点补齐 F2 的 Asset 发布进程中断，新增 8 个三进程场景。只修改测试、测试编排和中文文档；没有修改生产 AssetStore、SnapshotStore 或 Kernel API，没有引入三方库、垃圾回收策略或上层模块。

## 原子发布边界的实际测试方式

生产 `FilesystemAssetStore` 将资产信封交给 Windows `FilesystemSnapshotStore`：临时文件写完并 `FlushFileBuffers`，关闭句柄，然后以 `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)` 发布，最后验证内容摘要并返回 AssetRef。

测试专用 `kernel_file_crash_probe.cpp` 包含并编译同一份生产 SnapshotStore 源文件，仅把其中的 MoveFileExW 调用转到测试包装函数。测试可执行程序的对象定义满足静态库引用，不再抽取静态库中的原 SnapshotStore 对象。包装函数仍调用真实 Windows API，在调用前或成功后触发探针；不复制文件发布算法，不给生产 API 增加回调或测试开关。

探针只在本场景武装后、目标位于专用 assets 目录时触发；其他 Snapshot 发布仍正常委托。Debug/Release 的改名前后场景必须实际到达精确标记并以 86 退出，否则用例失败，不能用正常返回掩盖未命中。

## 8 个场景

| 场景 | 退出位置或附加故障 | 恢复断言 |
| --- | --- | --- |
| `asset-before-write` | handler 准备发布资产、调用 publish 前 | 无资产文件，文档仍为基线；显式重试后发布并提交一次 |
| `asset-before-rename` | 完整临时信封已写完、flush、关闭，真实 MoveFileExW 调用前 | 一个完整临时孤立文件，没有正式目标；AssetRef 读取为 NotFound，启动不采用临时文件 |
| `asset-after-rename` | 真实 MoveFileExW 成功后、publish 校验返回前 | 正式资产完整可验证，引用未提交，恢复只保留基线；显式重试复用同一不可变资产 |
| `asset-published` | publish 已返回验证后的引用，尚未创建事务对象候选 | 孤立正式资产允许，不能自动增加文档引用 |
| `asset-reference-inserted` | 含 AssetRef 的 Journal INSERT 后、SQLite commit 前 | 未提交 Journal/对象/History 不可见，完整孤立资产可复用 |
| `asset-reference-committed` | SQLite commit 成功、内存安装前 | 恢复目标对象及完全一致的 AssetRef；原请求重放回执，不重跑发布 handler |
| `asset-missing-committed` | 上一提交边界内，将正式文件移到专用证据文件后退出 | 启动 Asset.StateAdmissionFailed / Asset.NotFound，不安装文档，不重新执行；保留 Journal、历史和完成回执 |
| `asset-corrupt-committed` | 上一提交边界内，先备份原文件，再改变末字节并退出 | 启动 Asset.StateAdmissionFailed / Asset.DigestMismatch；不以伪造内容满足已提交引用 |

最后两条是“提交后文件丢失/损坏 + 进程退出”的交叉故障，不冒充普通进程终止会必然损坏磁盘，也不代表真实掉电测试。原始文件保留在该轮目录的 `asset-original.snapshot`，不会自动修复回去。

## 引用、事务与历史一致性

- 退出点始终检查活动 Document 仍为执行前对象/Revision；数据库提交成功后的领先状态交给新进程恢复。
- 发布前/引用提交前，恢复文档只有基线对象，Project/Document/Geometry Revision=1、History=1/1，claim abandoned；原 Key 显式重试只执行一次 handler。
- 引用提交后恢复为 Revision=2、History=2/2、claim completed；原 Key 的完整回执中保留 AssetRef 的 id/digest/kind/byteSize。
- 正向恢复用例实际执行 Undo/Redo，再次审计为 Revision=4、History=2/2；资产引用、对象 Value/类型/版本、逐条 Journal Revision 链不漂移。
- 资产内容包含 NUL 和非 ASCII 字节，正式文件读取必须逐字节相等。改名前的临时文件也检查完整信封和载荷，重试后仍保留原临时孤立文件，不能冒充发布文件或被静默消费。
- 丢失/损坏用例连续两个新进程都必须拒绝启动。只读持久恢复候选仍保留原引用和 Revision=2、两条历史材料及 completed 回执，但这些候选绝不能成为活动 Document。

本节点不增加自动垃圾回收或资产修复。孤立文件允许保留，已提交引用的校验不能因恢复便利而放宽。

## 测试编排安全

F2A 的每轮随机目录、三进程日志和 30 秒子进程超时继续沿用。早期 Persistence/Workflow 两条独立进程用例也改为绝对构建目录下的随机子目录，保留 stdout/stderr/退出码，不再递归删除调用者传入路径；每个子进程 30 秒、整条 CTest 70 秒。原始三进程用例整条 CTest 为 100 秒。

## 门禁与复现

```powershell
cmake --build --preset vs2022-debug --parallel 1 -- /nodeReuse:false
ctest --preset vs2022-debug -R 'integration.kernel_crash_asset-' --output-on-failure
ctest --preset vs2022-debug -R 'integration.kernel_crash_|integration.kernel_(persistence|workflow)_process_recovery' --repeat until-fail:20 --output-on-failure
```

本节点最终门禁：

- Debug/Release 警告错误构建通过；Debug 全集 238/238（63.48 秒），Release 全集 238/238（64.16 秒）。
- 当前版本的 27 个三进程场景及 2 个早期二进程用例各连续重复 20 次，共 580 次 CTest、1,700 个子进程全部通过（671.62 秒），没有 flaky、超时或不一致状态。
- Production-only Release 通过；31 个工程中测试/contract/Catch2 目标为 0，CTest 文件为 0，Catch2 源码目录不存在，测试探针未进入生产。
- 架构扫描通过 69 个公共头文件、133 个生产源文件；架构负例随全集通过。Git 差异和中文文档链接检查通过。
- 改名前中断的证据目录已抽查：正式资产与原临时文件均保留，退出码/标记正确，第三进程审计通过。

日志为 `build/k10f-f2c-debug-tests.log`、`build/k10f-f2c-release-tests.log`、`build/k10f-f2c-focused-tests.log`、`build/k10f-f2-repeat20.log` 与对应构建日志。F2 七类软件恢复范围的准入见 [F2 验收汇总](K10F-F2-独立进程恢复验收.md)，不代表完整 K10F 或 Frozen。

源码入口：`tests/integration/kernel_crash_contract.cpp`、`kernel_file_crash_probe.cpp`、`cmake/RunKernelCrashContract.cmake`。纯生产目标不编译测试探针；没有改变文件存储公共契约。

## 边界

测试证明指定软件边界上的进程终止恢复，不穷举 MoveFileExW 内部指令，不认证 OS/文件系统/硬盘断电行为。控制器、机械运动、激光及上层模块不在范围内；F3/F4/F5 仍需继续。
