# LaserCNC v3.0

LaserCNC v3.0 的 Application Kernel 已完成 Phase 1–9 及 K10A–K10E 的既有收口节点，具备版本化执行、文档生命周期、History、模块治理与 ExecutionGateway、对象类型和资产状态准入。K10F 的 F1–F5 工程门禁已形成固定版本检查点，当前继续补齐 ST1 独立项目生命周期，Kernel 1.0 尚未 Frozen。仓库坚持 Command-First、Automation-First、Headless-First 和 Infrastructure-Adapter；CAD、CAM、Machine、Process、Qt GUI、产品 RPC/CLI 与 AI 等上层模块仍未开始。

## 已完成阶段

当前收口按 [ST1C 补充执行计划](docs/内核契约/ST1C-补充审计与剩余执行计划.md) 继续：最新 [C6b14 资源声明枚举与算术准入](docs/阶段交付/2026-09-05-ST1C6b14-资源声明枚举与算术准入.md) 在配置、外部 Effect 注册和任务仲裁三处拒绝未知资源枚举，并阻止重复 Shared units 回绕后执行；C6b13 的编排定义枚举规则继续保留。下一步按 [C6 子节点](docs/内核契约/ST1C6-公共契约与输入预算.md) 继续 Host、其余执行、状态与持久族类型/格式，再完成统一预算和终态保留，之后 C7/C8 与 ST1D。整体尚未 Frozen，不扩展上层模块。

C5a/b 已完成持久化会话端口、SQLite/Windows 独占及强制初始化的本地检查点：C5b Debug 299/299、专项 56/56、新增 5 项各 10 次、纯生产及架构检查通过，见 [C5b 交付](docs/阶段交付/2026-09-04-ST1C5b-初始化独占与活动状态保护.md)。第二 Host 不再改写运行中 claim，隔离后仍保留所有权；C1b 已承接写入端修订检查。C2c 生命周期及 ST1D 最终签核仍未闭合，不宣称 Frozen。

C1b 已实现 Journal 写事务内的修订、持久头和首笔文档归属准入：Debug 308/308、专项 14/14、新增 9 项与扩展故障矩阵各 10 次通过，见 [C1b 交付](docs/阶段交付/2026-09-04-ST1C1b-Journal写入修订一致性.md)。失败不发布状态、History、事件或成功回执；C2 继续整体准入、Project-only 活动和生命周期收口。

C2a 已实现完整执行与生命周期写入口的共享准入，以及 shutdown 空闲检查与关闭准入的原子协调；保留被拒绝 Command/Query 的 trace/metrics。Debug 314/314、专项 7/7、7 项各 10 次通过，见 [C2a 交付](docs/阶段交付/2026-09-04-ST1C2a-整体准入与停止线性化.md)。后续继续 C2b 项目活动与 C2c drain/析构，不以本节点代替整体冻结。

C2b1 已实现 Project-only Command/Query 活动保护、无文档 Task 项目关闭桥接及终态发布保护：Debug 320/320、专项 13/13、13 项各 10 次及纯生产门禁通过，见 [C2b1 交付](docs/阶段交付/2026-09-04-ST1C2b1-项目活动与任务关闭桥接.md)。下一步 C2b2 生命周期控制/长期归属/legacy 恢复与 C2b3 目录失效；C2b 全项、C2c 和 ST1D 仍未签核。

C2b2a 已补编排实例真实归属、完整 advance/cancel 保护、未保存终态与文档关闭预检，Debug 324/324、20 项各 10 次及纯生产通过，见 [C2b2a 交付](docs/阶段交付/2026-09-04-ST1C2b2a-编排活动与关闭预检.md)。生命周期命令分类、终态/legacy 恢复、C2b3 和后续冻结节点仍未完成。

C2b2b 已补终态 Workflow 的持久归属恢复，允许查询 Detached/Closed/Removed 容器的历史而不重新打开或执行，Debug 328/328、14 项各 10 次及纯生产通过，见 [C2b2b 交付](docs/阶段交付/2026-09-04-ST1C2b2b-终态工作流历史恢复.md)。生命周期命令分类与 legacy Project-only Task/Effect 认证根仍待处理，整体未冻结。

C2b2c 已形成受治理固定生命周期命令及创建身份/墓碑保护的本地检查点：最终 Debug 339/339、专项 25/25、新增 11 项各 10 次及纯生产/架构通过，见 [C2b2c 交付](docs/阶段交付/2026-09-04-ST1C2b2c-受治理生命周期命令.md)。下一步继续 legacy Project-only Task/Effect 认证根、目录失效和 drain/析构；整体仍未冻结。

C2b2d 本地检查点通过：历史 Task/Effect 项目根已接入认证、一次性迁移与后续缺根拒绝；最终 Debug 347/347、专项 8/8、新增 8 项各 10 次和纯生产/架构通过，见 [C2b2d 交付](docs/阶段交付/2026-09-04-ST1C2b2d-历史执行项目根认证.md)。以上为按节点保留的历史进展，当前下一步为 C2b3 目录版本与失效，再推进 C2c 和后续冻结节点；仍仅覆盖内核，整体未冻结。

C2b3 本地检查点通过：Project/Document 原子目录快照及 epoch/作用域/修订失效已实现，最终 Debug 360/360、专项 13/13、13 项各 10 次和纯生产/架构通过，见 [C2b3 交付](docs/阶段交付/2026-09-04-ST1C2b3-目录版本与失效.md)。其中包含同库三进程重启，目录令牌不代表权限、活动租约或业务修订。后续进入 C2c，C3/C4/C6–C8 及 ST1D 仍完整保留，整体未冻结。

C2c1 本地检查点通过：停止确认/失败重试已修复，Debug 368/368、专项 8/8、停止相关 10 项各 10 次和纯生产/架构通过，见 [C2c1 交付](docs/阶段交付/2026-09-04-ST1C2c1-停止确认与失败重试.md)。下一步 C2c2 的生命周期线程、非协作任务析构依赖和持久化独占释放，见 [C2c 契约](docs/内核契约/ST1C2c-停止确认与析构依赖.md)；不将停止调用成功推导为任意析构路径安全，整体未冻结。

C2c2 后续检查点通过：最终排空与依赖保留、工作线程停止/销毁约束及所有权释放已验证，Debug 373/373、专项 15/15、15 项各 10 次、纯生产/架构和定向 ASan 进程矩阵 3 轮通过，见 [C2c2 交付](docs/阶段交付/2026-09-04-ST1C2c2-最终排空与析构依赖.md)。当前形成 C2 生命周期正确性大节点，下一步 C3 存储键；C4/C6–C8/ST1D 仍待完成，未宣称整体冻结或设备准入。

C3a/b/c 正确性检查点通过：快照身份和文件键解耦，公开读取与完整恢复统一锚点/对象状态/历史归属校验。最新 Debug 401/401、专项 10 项各 3 次、统一 C3 ASan 48/48 及另 10 项各 3 次、纯生产/架构通过，见 [C3c2 交付](docs/阶段交付/2026-09-04-ST1C3c2-统一快照锚点认证.md)。下一步 C4 公开 attach 与可信导入收口；C5 支持矩阵、C6–C8 和 ST1D 仍待完成，整体未冻结。

C4a/b/c 正确性检查点通过：[子节点契约](docs/内核契约/ST1C4-可信装载与事务导入.md) 区分基准迁移、公开入口封闭与失败验证。公开 attach 已移除，新增 5 项覆盖 22 个故障/重入场景；最终 Debug 407/407、统一定向 ASan 29 项各 3 次（87/87）、纯生产/真实编译/架构通过，见 [C4c 交付](docs/阶段交付/2026-09-04-ST1C4c-恢复失败准入与事务导入矩阵.md)。形成远端大节点，下一步 C5 支持矩阵与 C6–C8，最终 ST1D 和整体 Frozen 仍未签核。

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
