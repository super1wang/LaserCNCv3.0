# Phase 9 工作流与脚本运行时契约

## 阶段状态

Phase 9 已验收。本阶段只实现 Application Kernel 自有的 Workflow Runtime 与 Script Runtime，不实现 CAD/CAM/Machine/Process、Qt GUI、产品 CLI/RPC、AI Planner、控制器或任何上层领域模块。

蓝图要求 Workflow 支持 Variables、Dependency、Condition、Parallel、Wait、Timeout、Retry、Cancellation、Checkpoint 与 Compensation；Script 第一阶段支持 Command、Query、Variables、Result Binding、Wait、Assert、If、ForEach 与 Include。上述语义必须通过已有 CommandRuntime、QueryRuntime、TaskRuntime 和 PersistenceService 组合，不能引入新的业务状态写入入口。

当前已完成 Workflow 类型/Registry、有界推进器、Task 等待、重试/取消/补偿、SQLite v6 检查点、AppKernel 恢复、结构化 Script Runtime、全链路观测和独立进程恢复门禁。Debug、Release、重复性、Production-only 与架构扫描均已通过，Phase 9 可以作为上层模块建设前的内核基线。

C6b13 补充冻结准入：WorkflowRegistry 显式拒绝未知 WorkflowStepKind/WorkflowPredicateKind，ScriptRegistry 显式拒绝未知 ScriptNodeKind/WorkflowPredicateKind/ScriptWaitTarget；非法结构化定义不得进入注册表、引用解析、Freeze、实例执行或恢复。错误与兼容边界见 [编排定义枚举契约](ST1C6b13-编排定义枚举准入.md)。该增量不代签 DTO 内容预算、持久状态枚举或同步推进取消。

## 总体边界

```text
结构化 Script AST
        |
        v
ScriptRuntime ------> WorkflowRuntime
                          |
             +------------+------------+
             |            |            |
             v            v            v
       CommandRuntime  QueryRuntime  TaskRuntime
             |            |            |
             +------------+------------+
                          |
                 PersistenceService
```

- Workflow/Script 类型、状态机与错误语义由 Kernel 自己拥有，不使用 Taskflow、Temporal 或脚本语言对象定义公共契约。
- Script 不接触 DocumentStore、TransactionManager、Persistence backend、文件系统或 handler；所有系统访问只能走 Command、Query 或 Workflow。
- Kernel 第一阶段只接受结构化 Value/AST，不实现文本语法 parser。未来 JSON/YAML/DSL/Python/Lua/JavaScript 只能作为 Adapter，把输入转换为同一结构化契约。
- Workflow/Script 定义必须带名称和 Version；跨持久边界使用 WorkflowId、WorkflowStepId、ScriptName 等稳定身份。

## Workflow 定义

WorkflowDefinition 是注册后不可变的有向无环图：

- descriptor：WorkflowName、Version、输入/结果 Schema；
- steps：稳定 StepId、步骤类型、依赖、可选条件、超时、重试策略、结果绑定和可选补偿 Command；
- outputs：从变量表选择最终结果；
- 图在 Registry Freeze 前完成重复 ID、缺失依赖、循环依赖、非法超时/重试与绑定路径校验。

第一阶段步骤类型：

- Command：调用唯一 CommandRuntime，允许同步结果或异步 Task 接受；
- Query：调用唯一 QueryRuntime，只读获取不可变结果；
- WaitTask：从变量取得 TaskId，经 TaskRuntime 查询/等待其终态；
- Assign：把字面值或变量引用写入工作流变量；
- Assert：谓词为假时以明确 Error 终止；
- Barrier：只表达依赖汇合，不执行系统副作用。

Dependency DAG 表达 Parallel：一次 `advance()` 会按稳定 StepId 顺序启动全部已满足依赖的独立分支；异步 Command 产生的 Task 由现有 Scheduler 并行执行。WorkflowRuntime 不私建线程，也不绕过 Scheduler。

## 变量、引用与条件

- 实例变量为 Kernel Value::Object；输入在启动时经 descriptor Schema 校验后复制进入变量表。
- 参数模板递归识别显式变量引用节点，不执行字符串拼接或任意表达式求值；引用不存在、路径类型错误或目标绑定冲突均 fail-closed。
- Result Binding 只能写入声明的变量路径；步骤已成功后绑定不可被同一步骤的重试改写为不同值。
- 条件与 Assert 使用有限谓词模型：存在性、布尔值、相等/不等和数组非空；不嵌入通用语言运行时。
- 条件为假时步骤进入 Skipped，仍可作为依赖完成点；Skipped 步骤不执行补偿。

## 状态机与推进

工作流实例状态：

```text
Pending -> Running -> Waiting -> Running -> Succeeded
                    |             |
                    +-------------+-> Failed
                    +-------------+-> Cancelled
                    +-------------+-> Compensating
                                      |-> Compensated
                                      |-> CompensationFailed
```

步骤状态：Pending、Ready、Running、Waiting、Succeeded、Skipped、Failed、Cancelled、Compensated、CompensationFailed。

- `start()` 只创建实例和首个检查点，不在持久化成功前执行步骤。
- `advance()` 是有界、可重复调用的状态机推进：执行当前所有可运行步骤，遇到未完成 Task 或外部等待时返回 Waiting，不阻塞 Host 事件循环。
- WaitTask 只观察现有 TaskId；成功绑定 Task result，Failed/Cancelled/Stale 映射为工作流步骤失败。
- Workflow deadline 与 step timeout 在步骤执行前后检查。同步 handler 无法被强制抢占；超时只拒绝后续推进并触发取消/补偿。异步 Task 继续使用其自身 deadline 与协作取消。
- `cancel()` 记录取消意图，向已接受且未终止的 Task 请求协作取消，然后进入补偿；取消不是线程终止，也不直接修改 Document。

## Retry 与幂等

- RetryPolicy 明确最大尝试次数和确定性 backoff；第一阶段不在 Kernel 内 sleep，未到 `nextAttemptAt` 时实例保持 Waiting。
- 每次 Command 尝试使用由 WorkflowId、StepId 和 attempt 派生的稳定 IdempotencyKey。进程崩溃后，同一尝试经 Phase 8 持久幂等返回原 Commit/TaskId，不重放 handler 或 Event。
- Query 可安全重新执行，但其已完成结果仍写入检查点，正常恢复优先消费检查点。
- 非幂等 Command 不允许配置 Retry，也不允许作为可恢复的 Running 步骤；定义注册时拒绝这种组合。
- 错误是否可重试由显式 Error code allowlist 决定；未列出的错误直接失败。

## Checkpoint 与恢复

- PersistenceService schema v6 负责 `workflow_instances` 与 `workflow_steps`；定义本体不写入实例表，但实例保存 WorkflowName/Version 和定义摘要，防止恢复时静默换图。
- 首次接受、步骤进入执行前、步骤终态/变量绑定、等待、取消意图、补偿进度与实例终态都必须形成原子检查点。
- 每个检查点保存状态、变量、尝试次数、TaskId、deadline 身份、完成顺序、原 Error、补偿 Error 与强摘要。
- 恢复只加载与当前注册定义名称、Version、摘要完全匹配的实例；缺失定义、摘要损坏或图漂移必须 fail-closed。
- 崩溃时处于 Running 的 Command 依靠稳定 IdempotencyKey 重放同一尝试；无法证明可恢复的步骤必须进入明确 Failed/Compensating，不能猜测成功。
- 恢复不会自动执行。AppKernel Ready 后由 Host 或调用方显式 `advance()`，保持启动过程无业务副作用。

当前实现由 SQLite v6 `workflow_instances` 与 `workflow_steps` 在同一事务中更新。实例 payload 和每个步骤 payload 均单独计算 SHA-256，读取时同时核对控制面状态、Workflow/Step 身份、Version、定义摘要和完整步骤集合。步骤进入 Running 的检查点必须先成功，才能调用 Command/Query/Task；写入失败会恢复内存中的执行前状态，handler 调用次数保持为零。崩溃留下的 Running 主步骤在恢复后归一为 Waiting，设置同一 attempt 的重放标记；补偿步骤同理保留 compensation attempt，均不能生成新的幂等键。

## Compensation

- Compensation 是步骤显式声明的 Command 模板，不是 SQLite rollback，也不承诺恢复外部世界到原始状态。
- 只补偿已成功且声明补偿的步骤，严格按成功完成顺序逆序执行；Skipped/未开始步骤不补偿。
- 补偿 Command 使用由 WorkflowId、原 StepId 和 compensation attempt 派生的稳定 IdempotencyKey，可跨重启安全重放。
- 原始失败永远保留；补偿全部成功进入 Compensated，任一补偿最终失败进入 CompensationFailed，并同时保留原 Error 与补偿错误列表。
- Dangerous side effect 仍必须由原 Command 的 Capability 检查负责；Workflow 不提升权限、不代替确认或安全许可。

## Script Runtime

ScriptDefinition 是版本化结构化 AST，由 ScriptRegistry 在 Freeze 前注册。第一阶段节点：

- Command、Query：构造标准请求并调用现有 Runtime；
- Workflow：启动或推进已注册 Workflow；
- Wait：只等待前述调用返回的 TaskId/WorkflowId；
- Assign：设置脚本局部变量；
- Assert：有限谓词失败时返回明确 Error；
- If：根据有限谓词选择一个分支；
- ForEach：对 Value Array 按稳定索引顺序执行子节点，并提供 item/index 局部变量；
- Include：引用明确 ScriptName + Version，注册时检测缺失和循环依赖。

脚本变量具有词法作用域：Include/If 继承父作用域，ForEach 创建迭代局部作用域；只有显式 Result Binding 写回父作用域。最大 include 深度、循环次数和总执行节点数必须有界，防止无界展开。

Script Runtime 第一阶段是同步、有界的结构化解释器；遇到未完成 Task/Workflow 时返回可继续的进程内游标，而不是阻塞线程。Script 不建立第二套数据库表或状态写入链，也不宣称跨进程恢复；需要跨进程恢复的编排必须建模为 WorkflowDefinition。未来若要持久化 Script，必须复用 Workflow 的检查点与幂等契约，不能旁路另建持久化语义。

## 生命周期、观测与权限

- WorkflowRegistry/ScriptRegistry 只在 AppKernel Configuring 期注册，Ready 时 Freeze。
- Workflow/Script 请求沿用 SessionId、ProjectId、DocumentId、TraceId、CorrelationId 和调用者 Capability；子调用创建父子 Span，但不得改变安全主体。
- Runtime 停止接受新实例后，必须先完成已有 `advance()` 临界区；等待中的 Task 走协作取消和已有 Scheduler 有界关闭。
- 观测链为 `script.advance -> script.node -> workflow.advance -> workflow.step -> command/query/task`，沿用同一 TraceId 并建立父子 Span；步骤/节点 Metric 只使用 kind、outcome、compensation 等低基数标签。
- 每个 Workflow/Script 实例最多保留 256 个步骤/节点 Span，避免长循环挤占 LocalTraceService 的有界缓存；低基数聚合 Metric 仍覆盖全部执行。观测失败或达到 Span 预算不得改变业务状态机。

## 阶段验收门槛

- Registry：版本、Schema、重复、缺失依赖、DAG 循环、Include 循环和 Freeze；
- Workflow：变量绑定、条件、独立分支、Barrier、Task Wait、timeout、retry/backoff、cancel；
- Persistence：每一状态转换检查点、摘要篡改、定义漂移、独立进程中断与恢复；
- Compensation：逆序、幂等重放、原错误保留和补偿失败；
- Script：Command、Query、Workflow、Wait、Assert、If、ForEach、Include 与执行上限；
- 统一入口：脚本/工作流不得直接得到 Transaction、Document 可变引用、Persistence backend 或 handler；
- Debug/Release 全量、Debug 重复、Production-only、架构扫描与独立进程门禁；
- 中文契约、路线图与阶段交付文档同步更新。

## 验收结论

最终代码快照在 Windows、Visual Studio 2022、x64、MSVC 19.44.35216 与 Windows SDK 10.0.26100.0 下完成以下门禁：

- Debug：130/130 CTest；
- Release：130/130 CTest；
- Debug 全集连续 20 轮：2,600 次测试执行全部通过，141.12 秒；
- 独立进程 Workflow 恢复：Running 检查点后 `_Exit`，恢复后沿用 attempt 1，handler 与领域 Event 各发生一次，终态再次推进不重放；
- Production-only Release：31 个 VS 工程，测试/contract/Catch 类目标为 0，Catch2 source 不存在；
- 架构扫描：57 个 Kernel 公共头文件、108 个生产源文件通过。

完整证据见 [`../阶段交付/2026-09-03-Phase9-Workflow-Script.md`](../阶段交付/2026-09-03-Phase9-Workflow-Script.md)。
