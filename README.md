# LaserCNC v3.0

LaserCNC v3.0 的 Application Kernel 已完成 Phase 1–9 及 K10A–K10E 收口节点，具备版本化执行、文档生命周期、History、模块治理与 ExecutionGateway、对象类型和资产状态准入。K10F 可靠性认证中的 F1/F2 已验收，F3A 命令/查询并发压力已完成，下一步为 F3B 任务/工作流取消竞态与生命周期压力；Kernel 1.0 尚未 Frozen。仓库坚持 Command-First、Automation-First、Headless-First 和 Infrastructure-Adapter；CAD、CAM、Machine、Process、Qt GUI、产品 RPC/CLI 与 AI 等上层模块仍未开始。

## 已完成阶段

- Phase 1：Foundation（`StrongId`、`Value`、`Result`、`Error`、`Schema`）
- Phase 2：Application Composition（`AppKernel`、`ServiceRegistry`、`ModuleRuntime`）
- Phase 3：Infrastructure Adapters（日志、JSON、TOML、SQLite、线程池）
- Phase 4：Document、Revision 与 Application Transaction
- Phase 5：CommandRuntime、QueryRuntime 与 EventBus
- Phase 6：TaskRuntime、Scheduler、Cancellation 与 Resource Model
- Phase 7：Tracing、Metrics 与 Diagnostics
- Phase 8：Snapshot、Journal、SQLite Persistence 与 Crash Recovery
- Phase 9：Workflow Runtime 与结构化 Script Runtime
- Kernel 1.0 Closure：K10A–K10E 已验收；K10F 的 F1/F2 已验收，F3A 已完成；完整 F3、F4–F5 待闭合，不能宣布 Kernel Frozen
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
