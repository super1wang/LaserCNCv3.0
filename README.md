# LaserCNC v3.0

LaserCNC v3.0 的 Application Kernel 已完成 Phase 1–9 及 K10A–K10E 的既有收口节点，具备版本化执行、文档生命周期、History、模块治理与 ExecutionGateway、对象类型和资产状态准入。K10F 的 F1–F5 工程门禁已形成固定版本检查点，当前继续补齐 ST1 独立项目生命周期，Kernel 1.0 尚未 Frozen。仓库坚持 Command-First、Automation-First、Headless-First 和 Infrastructure-Adapter；CAD、CAM、Machine、Process、Qt GUI、产品 RPC/CLI 与 AI 等上层模块仍未开始。

## 已完成阶段

当前收口按 [ST1C 补充执行计划](docs/内核契约/ST1C-补充审计与剩余执行计划.md) 继续：C1a 非零 ProjectRevision 独立恢复已通过本地检查点（Debug 286/286、专项 16/16、新增 2 项各 3 次、纯生产通过），见 [C1a 交付](docs/阶段交付/2026-09-04-ST1C1a-项目修订独立恢复.md)。C5 与 C1b 已推进如下；后续按计划闭合整体准入、存储键、可信装载及冻结契约，历史门禁成绩不表示剩余新发现已解决。

C5a/b 已完成持久化会话端口、SQLite/Windows 独占及强制初始化的本地检查点：C5b Debug 299/299、专项 56/56、新增 5 项各 10 次、纯生产及架构检查通过，见 [C5b 交付](docs/阶段交付/2026-09-04-ST1C5b-初始化独占与活动状态保护.md)。第二 Host 不再改写运行中 claim，隔离后仍保留所有权；C1b 已承接写入端修订检查。C2c 生命周期及 ST1D 最终签核仍未闭合，不宣称 Frozen。

C1b 已实现 Journal 写事务内的修订、持久头和首笔文档归属准入：Debug 308/308、专项 14/14、新增 9 项与扩展故障矩阵各 10 次通过，见 [C1b 交付](docs/阶段交付/2026-09-04-ST1C1b-Journal写入修订一致性.md)。失败不发布状态、History、事件或成功回执；C2 继续整体准入、Project-only 活动和生命周期收口。

C2a 已实现完整执行与生命周期写入口的共享准入，以及 shutdown 空闲检查与关闭准入的原子协调；保留被拒绝 Command/Query 的 trace/metrics。Debug 314/314、专项 7/7、7 项各 10 次通过，见 [C2a 交付](docs/阶段交付/2026-09-04-ST1C2a-整体准入与停止线性化.md)。后续继续 C2b 项目活动与 C2c drain/析构，不以本节点代替整体冻结。

C2b1 已实现 Project-only Command/Query 活动保护、无文档 Task 项目关闭桥接及终态发布保护：Debug 320/320、专项 13/13、13 项各 10 次及纯生产门禁通过，见 [C2b1 交付](docs/阶段交付/2026-09-04-ST1C2b1-项目活动与任务关闭桥接.md)。下一步 C2b2 生命周期控制/长期归属/legacy 恢复与 C2b3 目录失效；C2b 全项、C2c 和 ST1D 仍未签核。

C2b2a 已补编排实例真实归属、完整 advance/cancel 保护、未保存终态与文档关闭预检，Debug 324/324、20 项各 10 次及纯生产通过，见 [C2b2a 交付](docs/阶段交付/2026-09-04-ST1C2b2a-编排活动与关闭预检.md)。生命周期命令分类、终态/legacy 恢复、C2b3 和后续冻结节点仍未完成。

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
