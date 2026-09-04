# ST1C6b19 Host 与 State 状态边界

## 结论与范围

本节点复核 Host、Project/Document Runtime、State 与对应生命周期持久化中的公开状态枚举。结论不是“所有枚举都应接收任意整数并返回 `Result`”，而是按真实可达入口分为只读观察、已验证写入材料和闭集访问器前置条件。源码复核没有发现需要修改生产实现的新缺陷；补齐的是 Document 生命周期未知持久状态的零变更回归，以及此前分散状态证据的统一契约登记。

本节点只触及 Application Kernel 的测试和中文文档，不增加 CAD、CAM、Machine、Process、GUI、产品 CLI/RPC/AI 或其他上层模块。

## Host 与 Runtime 观察状态

| 类型 | 值域 | 可达能力与冻结结论 |
| --- | --- | --- |
| `AppKernelState` | Configuring、Starting、Ready、Stopping、Stopped、Failed | 仅由 `AppKernel::state()` 返回，状态写入由 Host 生命周期私有实现持有。调用者不能把该枚举提交给 Host，也不能从观察到 Ready 推导持久所有权、执行许可或析构已完成 |
| `ModuleRuntimeState` | Configuring、Starting、Ready、Stopping、Stopped、Failed | 仅由 `ModuleRuntime::state()` 返回；启动、回滚和停止改变状态，不是持久 wire 或外部解码目标 |
| `ModuleState` | Discovered、Registered、Initialized、Started、Ready、Stopping、Stopped、Failed | 仅出现在 `ModuleSnapshot` 观察结果；`lastError` 保留模块失败证据，调用者不能写回快照改变模块 |
| `ProjectLifecycleState` | Closed、Opening、Open、Closing、Failed | `ProjectRuntime` 的 create/open/close 操作驱动，公开 lifecycle/list/catalog 只返回快照；名称函数对未定义值返回 `unknown`，但这不把未知值变成合法状态 |
| `DocumentLifecycleState` | Detached、Opening、Open、Closing、Failed | `DocumentRuntime` 的 create/open/close/detach/remove 驱动，公开 lifecycle/list/catalog 只返回快照；名称函数对未定义值返回 `unknown` |
| `DocumentActivityKind` | Command、Query、Transaction、TaskAdmission、WorkflowAdmission、ScriptAdmission | 用于 Runtime 友元之间的私有租约计数；公开头中的类型可见性不等于存在外部状态写入口 |
| `PersistenceOwnershipState` | NotRequested、Unconfirmed、Acquired | 仅由 `sessionStatus()` 返回。Acquired 是最近会话准入的观察证据，不是释放所有权、绕过隔离或重新执行恢复的授权令牌 |

这些 C++ 枚举的声明顺序和底层数字不是跨工具链 ABI 或持久协议。Host/Runtime 状态若将来进入外部协议，必须新建显式字符串/版本映射和输入准入，不能直接序列化内存枚举值。

## State 的闭集输入与访问器前置条件

`RevisionScope` 的闭集为 Project、Document、Geometry、Cam、MachineContext、Environment。可接收调用者材料的 `RevisionManager::validate()` 与 `advance()` 会先验证 scope，未定义值返回 `Revision.InvalidScope`，重复前置条件返回 `Revision.DuplicatePrecondition`；修订溢出返回 `Revision.Overflow`，不得产生部分新 `RevisionSet`。

`RevisionSet::at()` 是 `noexcept` 的直接闭集访问器，只接受已经验证或由内核构造的合法 `RevisionScope`。它不是反序列化或外部准入 API；传入未定义值违反前置条件并终止，不能为追求表面上的“任意输入均 Result”而静默返回零或任意槽位。所有外部整数、文本和持久材料必须先经过所属解码器或 `RevisionManager` 的闭集验证。`revisionScopeName()` 对未定义值返回 `unknown`，仅用于安全诊断，不代表准入成功。

`ObjectPersistencePolicy` 的闭集为 Durable、Transient。它是 `ObjectTypeDefinition` 的实际注册输入；`ObjectTypeRegistry::registerType()` 在发布定义前拒绝未定义值，错误为 `ObjectType.InvalidPersistencePolicy`，Registry 不增加条目。该检查与版本、迁移图、validator/reference 回调完整性共同构成注册准入，不能只检查枚举后忽略其余定义约束。

## Project / Document 生命周期与持久格式

Project 管稳定项目身份、生命周期、目录与整体活动协调，一个 Project 可拥有 0:N Document。Document 管自己的身份、运行时生命周期、对象状态、局部修订与 History；Module、视图和输入文件均不与 Document 强制一一对应。Project 关闭会协调子 Document，但两个 Runtime 的目录版本、状态快照和持久记录仍是独立事实，不能将 Project 状态复制成 Document 状态或反向推导业务修订。

`ProjectPersistenceState` 和 `DocumentPersistenceState` 与上述 Runtime 观察枚举不同，是 `PersistenceService::saveProjectLifecycle()` / `saveDocumentLifecycle()` 的公开写入材料：

- Project 闭集为 Closed、Opening、Open、Closing、Failed。未定义值在序列化和数据库事务前返回 `Persistence.InvalidProjectLifecycleState`，目录不变化。
- Document 闭集为 Detached、Opening、Open、Closing、Failed、Removed。未定义值在序列化、摘要和数据库事务前返回 `Persistence.InvalidDocumentLifecycleState`，既有目录行逐字段保持不变。
- Project 使用 `lasercnc.project-lifecycle.v1`，载荷绑定 `format/projectId/state/updatedAtMs`；Document 使用 `lasercnc.document-lifecycle.v1`，载荷绑定 `format/projectId/documentId/state`。数据库控制列、载荷和 SHA-256 摘要必须相互一致。
- 读取持久 Opening/Closing 时返回 Failed 且 `interruptedTransition=true`，用于暴露上次 Host 中断；读取本身不改写原记录，也不自动修复状态。
- 未知文本状态、字段/类型错误、摘要或控制列不一致均按持久基础设施损坏失败关闭，不能回退到默认 Closed/Detached/Open。

本节点不修改两个 v1 格式、SQLite schema、公共头或枚举数字，因此不存在新迁移步骤。合法既有记录的读取行为保持；历史损坏材料不会自动修复。数据库与快照/资产仍须成套回滚，不能只回退可执行文件来解释前向写入格式。

## 证据与未完成项

已有回归覆盖 AppKernel/Module 生命周期状态、失败回滚、Project/Document 中间态与失败态、Project 未定义持久状态、Revision 未定义 scope/重复/溢出及对象持久策略未定义值。本节点新增：

- Project/Document 生命周期名称对全部合法值给出稳定字符串，未定义值明确为 `unknown`；
- Document 生命周期持久入口拒绝 6 和 255 两个未定义底层值，并用独立 SQLite 观察连接确认既有行完全不变。

该证据只签核本文件列出的状态边界，不代签所有 Host 方法、State 的 `Value`/对象集合预算、其他 Persistence DTO/格式、跨工具链 ABI、同步取消/deadline 或终态保留。后续按 C6b20 继续剩余 Persistence 格式与 DTO，随后完成 C6b 全清单复核、C6c 统一预算和 C6d 同步/Task/有界终态。
