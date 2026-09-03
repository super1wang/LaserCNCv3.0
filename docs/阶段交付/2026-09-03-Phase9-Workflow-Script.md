# 2026-09-03 Phase 9 Workflow Runtime 与 Script Runtime 交付

## 结论

Phase 9 已验收。Application Kernel 已具备版本化 Workflow DAG、显式有界推进、Task 等待、重试、取消、持久检查点、崩溃恢复、逆序补偿，以及只经 Command/Query/Workflow 访问系统的结构化 Script Runtime。Workflow/Script 已纳入统一 Trace/Metric 链，独立进程门禁证明 Running 检查点可在不增加 attempt、不重复副作用的前提下恢复。

本阶段只修改 Kernel、Infrastructure 持久化适配、自动化测试和中文文档，没有创建或扩展 CAD、CAM、Machine、Process、Qt GUI、产品 CLI/RPC、AI Planner、控制器或其他上层模块。

## 交付内容

1. Workflow 定义与注册边界
   - 新增 WorkflowName、WorkflowId、WorkflowStepId、WorkflowDefinition、WorkflowSnapshot 与步骤状态；
   - WorkflowRegistry 在 Ready 前校验步骤形状、重复身份、缺失依赖、DAG 循环、精确 Command/Query 版本和 Command 幂等声明；
   - Registry Freeze 后定义不可变，持久实例使用名称、Version 与完整定义摘要防止静默换图。
2. Workflow 有界运行时
   - `startWorkflow()` 只创建实例并写首个检查点，`advance()` 显式推进，不私建线程、不阻塞 Host 事件循环；
   - 支持变量模板、结果绑定、条件跳过、Assign、Assert、Barrier、独立异步分支与 WaitTask；
   - Retry 只接受错误码 allowlist，不在 Kernel 内 sleep；deadline、step timeout 与取消均为协作语义；
   - Command 尝试使用 WorkflowId、StepId、attempt 派生的稳定 RequestId/IdempotencyKey。
3. 检查点与崩溃恢复
   - SQLite schema v6 增加 `workflow_instances`、`workflow_steps` 与索引，并在同一事务更新实例和完整步骤集；
   - 实例 payload、每个步骤 payload 与定义分别计算 SHA-256，读取时校验身份、Version、控制面状态、摘要和步骤集合；
   - 步骤先持久化 Running 才调用 handler；检查点失败恢复内存前态，handler 调用数为零；
   - 崩溃留下的 Running 主步骤恢复为 Waiting 并重用原 attempt，补偿步骤保留 compensation attempt；恢复不自动推进。
4. Retry、取消与补偿
   - 独立分支统一进入现有 TaskRuntime/Scheduler；等待只观察已存在 Task，不创建旁路执行器；
   - 取消意图与 Task 协作取消进入检查点，不能强制终止同步 handler；
   - 只补偿已成功且声明补偿的步骤，严格按完成顺序逆序执行；
   - 补偿 Command 使用稳定幂等身份，补偿失败同时保留原错误与补偿错误材料。
5. 结构化 Script Runtime
   - 新增 ScriptName、ScriptExecutionId、ScriptNodeId、ScriptDefinition、ScriptRegistry、ScriptRuntime 与 Snapshot；
   - 支持 Command、Query、Workflow、Wait、Assign、Assert、If、ForEach、Include；
   - 参数和值模板只识别显式 `$ref`，不执行字符串代码、动态表达式或任意脚本；
   - Include 使用精确 ScriptName + Version 并检测循环；ForEach 提供 item/index 局部变量；
   - include 深度、定义节点数、循环次数和总执行节点数均有上限；未完成 Task/Workflow 返回 Waiting 游标。
6. 统一入口与持久边界
   - Script 不获得 DocumentStore、ApplicationTransaction、Persistence backend、文件系统或 handler；
   - Script/Workflow 的 Command 和 Query 调用继续经过 Schema、Capability、Revision、事件与幂等规则；
   - 第一阶段 Script 游标仅在进程内，不新建 Script 持久表；需要跨进程恢复的编排必须建模为 Workflow。
7. 可观测性与生命周期
   - Trace 层级为 `script.advance -> script.node -> workflow.advance -> workflow.step -> command/query/task`；
   - 子调用保持原 Session/Project/Document/Trace/Correlation，不改变安全主体；
   - Workflow/Script completed 与 duration Metric 只使用 kind、outcome、compensation 等低基数标签；
   - 每个实例最多保留 256 个步骤/节点 Span，避免长循环占满有界 Trace 缓存；Metric 仍聚合全部执行；
   - AppKernel 在 Ready 前冻结两类 Registry，按 Script -> Workflow -> Command/Query/Task 顺序停止接受推进，并拒绝活动推进临界区内关闭。

## 独立进程恢复门禁

新增 `integration.kernel_workflow_process_recovery`，使用两个真正独立的进程：

```text
播种进程
  -> 建立空 Document Snapshot
  -> startWorkflow 写 Pending 检查点
  -> 写入 step=Running、attempt=1 检查点
  -> 确认 handler 调用数为 0
  -> std::_Exit 跳过正常析构

恢复进程
  -> AppKernel Ready 前加载并校验 Workflow
  -> Running 归一为 Waiting，attempt 仍为 1
  -> 显式 advance，handler 与 Domain Event 各发生 1 次
  -> 查询持久对象确认结果
  -> 再次 advance，handler/Event 计数保持 1
```

该门禁证明恢复材料来自持久介质，并验证“写前检查点 + 稳定幂等身份 + 显式恢复推进”的组合。它不把正常析构、同进程内存或测试 fixture 当作进程恢复证据。

## 自动化证据

环境：Windows、Visual Studio 2022、x64、MSVC 19.44.35216、Windows SDK 10.0.26100.0。

```powershell
cmake --build --preset vs2022-debug --parallel 16
ctest --preset vs2022-debug --output-on-failure

cmake --build --preset vs2022-release --parallel 16
ctest --preset vs2022-release --output-on-failure

ctest --preset vs2022-debug --repeat until-fail:20 --output-on-failure

cmake -S . -B build/production-only -G "Visual Studio 17 2022" -A x64 `
  -DLCNC_BUILD_TESTING=OFF -DLCNC_WARNINGS_AS_ERRORS=ON
cmake --build build/production-only --config Release --parallel 16

cmake -DLCNC_SOURCE_ROOT=J:/Code/LaserCNCv3.0 `
  -P cmake/VerifyKernelBoundaries.cmake
```

最终结果：

- Debug：130/130 CTest 通过；
- Release：130/130 CTest 通过；
- Debug repeat：130 项连续 20 轮通过，共 2,600 次测试执行，141.12 秒；
- 两项独立进程恢复门禁均包含在 Debug、Release 与重复测试中；
- Production-only Release：构建通过，31 个生成的 VS project 中测试/contract/Catch 类目标为 0，`_deps/catch2-src` 不存在；
- 架构扫描通过 57 个 Kernel 公共头文件和 108 个生产源文件；
- `git diff --check` 通过。

## 覆盖重点

- Registry 重复、形状、缺失引用、版本漂移、DAG/Include 循环与 Freeze；
- Workflow 变量、条件、Barrier、并行 Task、Wait、retry、deadline、cancel 与逆序补偿；
- 检查点原子性、强摘要篡改、定义/步骤漂移、Running/compensation attempt 恢复与持久化故障注入；
- Script Command、Query、Workflow、显式 Wait、Assert、If、ForEach、Include、局部变量与 10,000 节点上限；
- Script/Workflow/Step/Node/Command/Query 的父子 Span 和低基数 Metric；
- 独立进程异常退出后的同 attempt、单 handler、单 Event 与终态无重放。

## 已知边界与后续

本交付不实现通用脚本语言、文本 parser、Python/Lua/JavaScript VM、Temporal/Taskflow、分布式 Workflow、多 Host lease、远程 Workflow 调度或 Script 跨进程游标。同步 handler 不能被强制抢占；超时和取消只阻止后续推进并触发协作取消/补偿。外部不可逆副作用仍必须由 Command 自身的 Capability、确认和安全策略负责，补偿不是数据库 rollback。

Phase 9 是设计蓝图中上层模块之前的 Application Kernel 收口点。本阶段不证明 CAD/CAM/Machine/Process、GUI、产品 CLI/RPC、AI、控制器 SDK、真实设备或物理机能力；Phase 10 及以后保持延后，等待新的明确授权。
