# K10F-F5B：Host 状态写入口审计

## 当前状态

固定版本实现检查点已通过三配置全集、纯生产及性能重测；最终 F5 重复认证与补充终审尚未完成。F5 冻结审计在 `48f39a9` 发现两个仍可由普通 Host 代码调用的底层写入口。以下两节保留修复前的证据，修复及测试迁移见后续章节。本记录不撤销历史节点的测试结果，也不把它们扩大为完整冻结通过。

## 1. 修复前：History 恢复入口

`include/lasercnc/kernel/app_kernel.hpp` 的非 const `history()` 返回 `HistoryRuntime&`；`include/lasercnc/runtime/history_runtime.hpp` 仍公开 `restore(span<TransactionCommit>)`。`src/runtime/history/history_runtime.cpp` 的 restore 构造候选后直接替换 `histories_`；空集合也成功，没有 Ready/Capability/Transaction 准入。

因此 Host 可以调用 `kernel.history().restore({})` 清空活动历史，而没有通过 `edit.undo` / `edit.redo`，也没有同步形成 Journal、Document 或 Revision 变化。此结论来自可达接口和实现的源码审计；本节点尚未增加运行时复现或修复验证。K10C 文档中“Host 没有直接移动 HistoryCursor 的接口”不能覆盖这条整体替换路径。

计划收紧 AppKernel 的 History 暴露为只读观察；内核恢复仍由 AppKernel 内部持有的实例执行。增加类型级反例，保证普通 Host 表达式不能调用 restore；重跑 Undo/Redo、恢复、幂等及故障矩阵，保留独立组件测试所需的合法恢复能力。

## 2. 修复前：Persistence 底层写入口

非 const `AppKernel::persistence()` 返回 `PersistenceService&`，其中公开 append、claim/complete、acceptTask、saveWorkflowCheckpoint、captureSnapshot 与生命周期写入等底层操作。Ready 后的 freeze 保护配置变更，不会禁止实际运行所需的持久化写入。Host 因而可以越过 Command/Capability/Transaction，直接构造持久化材料。

已有测试也证明该入口可达：`persistence_service_tests.cpp` 通过 `kernel.persistence().append(injected)` 注入材料；早期无界面恢复契约通过 acceptTask/saveWorkflowCheckpoint 写入合成运行态。后两类是旧二进程测试，不是 F2 新增真实 handler 三进程证据，不应为兼容测试而保留生产 Host 写入口。

计划将 Host 可见接口拆成显式、仅配置期允许的持久化装配与运行期只读观察，底层写权限保留在内核组件之间。测试中的低层材料构造应使用独立组件或明确的测试夹具，而不能增加生产测试开关、公共可变访问后门或 const_cast 绕过。已有故障注入、27 个真实三进程恢复场景及 F4 测量含义必须保留；若调整基准实现或内核二进制，旧 F4 摘要不能继续声称与新版本一致。

## 3. 本次实现

移除 AppKernel 的两个可变 getter 重载，只保留 `const HistoryRuntime& history() const` 和 `const PersistenceService& persistence() const`；非 const Host 调用也得到只读引用。AppKernel 内部成员和正常执行链仍可访问原组件，独立 History/Persistence 组件的合法恢复、写入方法不删除。

新增 `AppKernel::configurePersistence(backend, serializer, hashes, snapshotStore)`，仅 `Configuring` 状态可委托底层配置；Starting、Ready、Stopped、Failed 均返回 `Kernel.PersistenceConfigurationClosed`。配置失败不替换原服务，原组件的参数检查、单次装配及 freeze 仍保留。没有增加可供调用方保留可变引用的 callback 或配置对象。

`module_runtime_tests.cpp` 的类型断言同时验证普通 Host 与 const Host 不能调用 History restore、Journal append、Task accept、Workflow checkpoint 或 Persistence initialize；正对照验证独立组件上这些方法真实存在。运行时用例检查启动回调中的装配拒绝和终态拒绝；已有真实 SQLite 配置用例继续验证正常装配及 Ready 后重新配置失败。

架构扫描增加两个可变 getter 的拒绝规则；新增隔离扫描夹具，以实际头文件为健康对照，分别恢复可变 History/Persistence 声明，要求扫描准确拒绝。扫描反例是文本规则验证；C++ 类型断言才负责真实类型可达性，不能相互冒充。

## 4. 测试迁移与证据边界

- 所有 AppKernel 持久化装配改走显式 configurePersistence；生产运行代码仍使用内部成员，不添加可变状态后门。
- 新增测试专用 `persistence_fixture.hpp`，只创建独立连接与独立 PersistenceService，不接受 AppKernel，不使用 friend 或 const_cast；默认 Snapshot 上限保持 1 MiB，Benchmark 显式保留 256 MiB。
- 不支持对象版本的 durable 注入、早期两个二进程契约的合成 Task/Workflow 记录，由独立组件构造；它们仍标为低层材料测试，27 个真实 handler 三进程场景保留原故障点和恢复断言。
- Snapshot 九阶段、返回错误/异常共十八场景迁移到夹具自有的故障组件；Kernel 继续通过另一连接观察未变的对象、Revision、History 与 Journal，并在新实例恢复后验证孤立文件复用。底层 Snapshot 写故障与真实 Document close 元数据故障是不同入口，后者仍在 AppKernel 内部组件注入，不能互相替代。
- Kernel 回滚失败用例继续检查隔离、后续命令拒绝及新实例恢复；Host 不再具有 initialize 路径。原底层 `Persistence.BackendQuarantined` 重初始化断言移至新增独立组件用例，分别触发 rollback 返回错误/抛异常，并检查独立连接看不到失败 Journal。
- Benchmark 的数据、操作采样与断言保留；基线 Snapshot 初始化改由独立连接完成，新增连接初始化计入 seed/lifecycle 启动时间。这会改变基准二进制及生命周期初始化口径，必须重新记录正式基线，不再宣称与 F4 原二进制摘要一致。

先行 Debug 专项 45/45 通过（47.49 秒），日志 `build/k10f-f5b-focused-tests.log`，涵盖受影响的集成契约、Snapshot 故障、回滚隔离、对象准入与 Host 配置。此后补齐独立夹具上限参数及架构负例；最终版本 Debug/Release/ASan 全集、重复矩阵和新基线仍待完成。先行结果不替代最终版本。

补齐上述修改后，Debug 全集 262/262 通过（313.94 秒），日志 `build/k10f-f5b-debug-tests.log`；其中新增 Host 配置用例、独立组件隔离用例和架构负例均通过。Debug 构建保持警告即错误，日志 `build/k10f-f5b-debug-final-build.log`。据此形成本地实现检查点；Release、ASan、Production-only、重复矩阵与正式性能重测仍待完成，不提前验收 F5B。

## 5. 验收要求

### 固定版本结果

实现提交 `68580d7a973fa15189078077df95edc3a4a78c8f`，2026-09-04 顺序完成以下门禁，未混入后续修正：

| 配置/门禁 | 实际结果 | 日志 |
| --- | --- | --- |
| Debug | 262/262；313.94 秒 | `build/k10f-f5b-debug-tests.log` |
| Release | 262/262；307.15 秒 | `build/k10f-f5b-release-tests.log` |
| ASan RelWithDebInfo | 265/265；312.10 秒 | `build/k10f-f5b-asan-tests.log` |
| Production-only Release | 31 个生成工程；0 测试/契约/Benchmark/ASan 工程、0 插桩开关、0 CTest 文件 | `build/k10f-f5b-production.log` |
| 架构扫描 | 69 个公共头、133 个生产源通过；三全集包含 Host 与 OCCT 负例 | 同上及各全集日志 |

三个全集均逐项核对了 Passed 行数及不同编号数量。对应日志 SHA-256：Debug `BE689BFF93589667438FA6B69BC5D5D90629B5C1AAC7978F854EF45F2A2A6599`；Release `DFD3E40D60445AA548ACFB02F3047723FBB055B154C1E3FFDB1BA643688D5A5E`；ASan `191B30E29653642331BD9FD050310AEE19719BE6ACE8D5D7637D83CB00A026EF`。

普通 Release 正式重测在干净工作区完成，索引为 `build/kernel-baselines/run-20260904-081420-7007e9fea61342c090660c06abd466a4/index.json`，状态 verified；21 份报告全部核对 SHA-256，合计 81 个操作项目、405 个样本及 60 轮生命周期。采集时间 08:14:22–08:15:59，数据规模仍为 1k/10k/100k，预热 1、样本 5、生命周期每组 10。程序 SHA-256 为 `382D0D518582AA1A480120DD134B67EB16BDEDDD9FB03760F118A9CE34018150`，不再与 F4 原程序等同。

此重测是 F5B 版本证据，原始材料保留在本机构建目录。后续生产修正将改变二进制，应对最终版本再采集并归档，不以这一中间版本签发最终性能基线。各次测量有正常波动，不能据单轮中位数断言没有性能回退、业务 SLA 或零泄漏。

### 最终仍需闭合

补充终审的 Scheduler 只读边界、未知 ContractStatus 拒绝、scope 返回值断言见 [F5C 记录](K10F-F5C-执行边界与状态终审.md)。以下门禁须在这些修正后的统一版本重新满足，不因 F5B 单轮通过而省略：

- 非 const Host 同样只能观察 History 与持久化运行态；合法配置入口在 Ready 后拒绝变更。
- 类型/编译反例覆盖 restore、append、Task 接受和 Workflow checkpoint 等代表性旁路，不仅测试返回错误。
- 正常 Gateway 执行、DocumentRuntime 生命周期、事务提交、Undo/Redo、持久幂等和恢复仍可用。
- 完成后重新固定代码版本并跑 Debug、Release、ASan、Production-only、架构门禁和最终重复矩阵；`48f39a9` 的结果只是修复前工具链/测试门禁证据。

本节点只收紧既有内核所有权和访问边界，不增加上层模块。独立 Project 生命周期范围确认是另一项未闭合要求，不能用本节点代替。
