# LaserCNC v3.0

LaserCNC v3.0 的 Application Kernel 已完成 Phase 1–9 及 K10A–K10E 的既有收口节点，具备版本化执行、文档生命周期、History、模块治理与 ExecutionGateway、对象类型和资产状态准入。K10F 的 F1–F5 工程门禁已形成固定版本检查点，当前继续补齐 ST1 独立项目生命周期，Kernel 1.0 尚未 Frozen。仓库坚持 Command-First、Automation-First、Headless-First 和 Infrastructure-Adapter；CAD、CAM、Machine、Process、Qt GUI、产品 RPC/CLI 与 AI 等上层模块仍未开始。

## 已完成阶段

当前收口按 [ST1C 补充执行计划](docs/内核契约/ST1C-补充审计与剩余执行计划.md) 继续：C1a 非零 ProjectRevision 独立恢复已通过本地检查点（Debug 286/286、专项 16/16、新增 2 项各 3 次、纯生产通过），见 [C1a 交付](docs/阶段交付/2026-09-04-ST1C1a-项目修订独立恢复.md)。下一步 C5 单 Host/耐久配置、C1b 写入端修订检查，再按计划闭合整体准入、存储键、可信装载及冻结契约；历史门禁成绩不表示剩余新发现已解决。

- Phase 1：Foundation（`StrongId`、`Value`、`Result`、`Error`、`Schema`）
- Phase 2：Application Composition（`AppKernel`、`ServiceRegistry`、`ModuleRuntime`）
- Phase 3：Infrastructure Adapters（日志、JSON、TOML、SQLite、线程池）
- Phase 4：Document、Revision 与 Application Transaction
- Phase 5：CommandRuntime、QueryRuntime 与 EventBus
- Phase 6：TaskRuntime、Scheduler、Cancellation 与 Resource Model
- Phase 7：Tracing、Metrics 与 Diagnostics
- Phase 8：Snapshot、Journal、SQLite Persistence 与 Crash Recovery
- Phase 9：Workflow Runtime 与结构化 Script Runtime
- Kernel 1.0 Closure：K10A–K10E 已验收；K10F 的 F1–F4 已验收；F5 待闭合，不能宣布 Kernel Frozen

F5A/B/C 工程加固检查点已验收：隔离 ASan 与真实探针、进程退出门禁、History/Persistence/Scheduler 的 Host 只读边界、未知契约状态拒绝及四 scope 输出断言均已闭合。固定实现 `0cbd348` 的 Debug/Release 各 264/264、ASan 267/267，全集 792 次、故障矩阵 180 次、独立进程恢复 580 次连续执行全部通过；21 份基线报告已归档。见 [F5C 记录](docs/内核契约/K10F-F5C-执行边界与状态终审.md) 和 [阶段交付](docs/阶段交付/2026-09-04-K10F-F5-工程加固与最终门禁.md)。

复核原规划后，ST1 按完整目标补齐独立 Project 生命周期，不再等待“是否实现”的额外决策，也不缩减为 Document ownership。ST1A 版本化持久目录与迁移基础已通过组件门禁；ST1B 的 ProjectRuntime、空项目、文档联动和关闭协调已通过本地检查点：Debug 284/284、项目用例 27 次连续验证及纯生产构建。Project-only 执行准入、完整恢复与最终认证仍由 ST1C/D 完成；见 [ST1B 交付](docs/阶段交付/2026-09-04-ST1B-项目生命周期与文档关闭协调.md)、[ST1 收口契约](docs/内核契约/ST1-独立项目生命周期收口.md) 与 [逐项审计清单](docs/内核契约/Kernel-1.0-冻结审计清单.md)。F5C 的旧固定版本成绩不替代 ST1 新代码的最终门禁。

- 标准：C++20
- 测试：Catch2 + CTest

详细状态见 [内核开发路线图](docs/内核开发路线图.md)，强制边界见
[内核架构规则](docs/内核架构规则.md)。原始设计依据保存在
[最终架构设计方案](LaserCNC%20Application%20Kernel%20最终架构设计方案.md)，收口范围见
[Kernel 1.0 最终收口设计规划](docs/Kernel%201.0%20最终收口设计规划.md)。

## Windows 构建

```powershell
cmake --preset vs2022-debug
cmake --build --preset vs2022-debug --parallel
ctest --preset vs2022-debug
```

构建产物位于 `build/vs2022`，该目录不进入版本控制。
