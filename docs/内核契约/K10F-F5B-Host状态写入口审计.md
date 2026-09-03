# K10F-F5B：Host 状态写入口审计

## 当前状态

待修复、未验收。F5 冻结审计在 `48f39a9` 发现两个仍可由普通 Host 代码调用的底层写入口。既有 Gateway 已收紧 TaskRuntime、TransactionManager 与可变 DocumentStore，但不能据此推导所有持久状态写入口都已封闭。本记录不撤销历史节点的测试结果，也不把它们扩大为完整冻结通过。

## 1. History 恢复入口

`include/lasercnc/kernel/app_kernel.hpp` 的非 const `history()` 返回 `HistoryRuntime&`；`include/lasercnc/runtime/history_runtime.hpp` 仍公开 `restore(span<TransactionCommit>)`。`src/runtime/history/history_runtime.cpp` 的 restore 构造候选后直接替换 `histories_`；空集合也成功，没有 Ready/Capability/Transaction 准入。

因此 Host 可以调用 `kernel.history().restore({})` 清空活动历史，而没有通过 `edit.undo` / `edit.redo`，也没有同步形成 Journal、Document 或 Revision 变化。此结论来自可达接口和实现的源码审计；本节点尚未增加运行时复现或修复验证。K10C 文档中“Host 没有直接移动 HistoryCursor 的接口”不能覆盖这条整体替换路径。

计划收紧 AppKernel 的 History 暴露为只读观察；内核恢复仍由 AppKernel 内部持有的实例执行。增加类型级反例，保证普通 Host 表达式不能调用 restore；重跑 Undo/Redo、恢复、幂等及故障矩阵，保留独立组件测试所需的合法恢复能力。

## 2. Persistence 底层写入口

非 const `AppKernel::persistence()` 返回 `PersistenceService&`，其中公开 append、claim/complete、acceptTask、saveWorkflowCheckpoint、captureSnapshot 与生命周期写入等底层操作。Ready 后的 freeze 保护配置变更，不会禁止实际运行所需的持久化写入。Host 因而可以越过 Command/Capability/Transaction，直接构造持久化材料。

已有测试也证明该入口可达：`persistence_service_tests.cpp` 通过 `kernel.persistence().append(injected)` 注入材料；早期无界面恢复契约通过 acceptTask/saveWorkflowCheckpoint 写入合成运行态。后两类是旧二进程测试，不是 F2 新增真实 handler 三进程证据，不应为兼容测试而保留生产 Host 写入口。

计划将 Host 可见接口拆成显式、仅配置期允许的持久化装配与运行期只读观察，底层写权限保留在内核组件之间。测试中的低层材料构造应使用独立组件或明确的测试夹具，而不能增加生产测试开关、公共可变访问后门或 const_cast 绕过。已有故障注入、27 个真实三进程恢复场景及 F4 测量含义必须保留；若调整基准实现或内核二进制，旧 F4 摘要不能继续声称与新版本一致。

## 验收要求

- 非 const Host 同样只能观察 History 与持久化运行态；合法配置入口在 Ready 后拒绝变更。
- 类型/编译反例覆盖 restore、append、Task 接受和 Workflow checkpoint 等代表性旁路，不仅测试返回错误。
- 正常 Gateway 执行、DocumentRuntime 生命周期、事务提交、Undo/Redo、持久幂等和恢复仍可用。
- 完成后重新固定代码版本并跑 Debug、Release、ASan、Production-only、架构门禁和最终重复矩阵；`48f39a9` 的结果只是修复前工具链/测试门禁证据。

本节点只收紧既有内核所有权和访问边界，不增加上层模块。独立 Project 生命周期范围确认是另一项未闭合要求，不能用本节点代替。
