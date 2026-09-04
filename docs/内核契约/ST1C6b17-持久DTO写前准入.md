# ST1C6b17 持久 DTO 写前准入

## 契约

Application Kernel 的持久化写入口不得把未定义枚举编码为 `"unknown"` 或退化为某个合法默认值，再等待读取或重启恢复时报错。Journal、Task 与 Workflow 的公开 DTO 在序列化、摘要计算和数据库事务之前完成闭集与结构验证；拒绝属于调用材料的 Validation 错误，不得建立 Journal、Task acceptance/terminal 或 Workflow checkpoint 记录。

Journal `append` 只接受 Created、Updated、Removed 三种 ObjectChangeKind，并核对 before/after 形状、ObjectId 一致性以及 Updated 前后 ObjectTypeId 一致性。HistoryMutationKind 只接受 None、Record、Barrier、Undo、Redo；Record 必须携带 command/version，Undo/Redo 必须携带 target/cursor，None/Barrier 不得夹带上述字段。分别使用 `Persistence.InvalidJournalChangeKind`、`Persistence.InvalidJournalChange`、`Persistence.InvalidJournalHistoryKind`、`Persistence.InvalidJournalHistory`。

Task acceptance 对每个 ResourceClaim 显式验证 ResourceKind 与 ResourceAccess，未知值分别返回 `Persistence.InvalidTaskResourceKind`、`Persistence.InvalidTaskResourceAccess`，不能把未知 access 当作 Shared。终态写入先区分未知 TaskState 与合法非终态：前者返回 `Persistence.InvalidTaskState`，后者继续使用 `Persistence.TaskNotTerminal`；持久 terminal error 的 category/severity 未定义时返回 `Persistence.InvalidTaskError`。

Workflow definition digest 与 checkpoint 写入共同验证定义中的 WorkflowStepKind 和可选 WorkflowPredicateKind；未知值返回 `Persistence.InvalidWorkflowDefinition`。Checkpoint 的 WorkflowState、每个 WorkflowStepState 分别由 `Persistence.InvalidWorkflowState`、`Persistence.InvalidWorkflowStepState` 拒绝。Workflow 顶层、步骤、补偿错误及其既有有界 cause 链中的未知 ErrorCategory/Severity 返回 `Persistence.InvalidWorkflowError`。

## 兼容与边界

本节点不改 71 个公共头、枚举数字、SQLite schema、Journal v4、Task acceptance/terminal v1、Workflow definition/checkpoint/step v1 或合法 payload 字段。合法 DTO 的序列化字节及摘要保持不变；已持久的非法历史材料不自动修复，读取和恢复仍按既有规则 fail-closed。

Task terminal 继续沿用既有顶层 Error 持久字段，本节点不新增 Task cause wire；ErrorCode/文本/Value、DTO 数量与深度、状态组合语义和终态保留总量仍由后续 C6b/c/d、C7 审计。Workflow 定义的完整结构规则仍由 Registry 负责，本节点只补持久组件对枚举闭集的独立防御，不把直接构造 PersistenceService 等同于 Host 业务写权限。

本节点只触及 Application Kernel 的持久化实现、测试和中文文档，不增加 CAD、CAM、Machine、Process、GUI、产品 CLI/RPC/AI 或其他上层模块。
