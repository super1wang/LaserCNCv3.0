# Phase 4 文档、修订与应用事务

状态：已验收。

Phase 4 建立 Kernel 自有的业务状态与一致性边界，不引入领域对象、GUI、CommandRuntime、QueryRuntime、EventBus、持久化编排或第三方框架。

## 状态所有权

- `ProjectId`、`DocumentId`、`ObjectId`、`ObjectTypeId`、`TransactionId` 均为独立 StrongId，不能互相混用。
- `DocumentStore` 统一拥有活动文档的可变状态，以及同一项目内跨文档共享的 ProjectRevision。
- 对外只返回按值复制的 `Document` 不可变快照；快照包含稳定身份、RevisionSet 和只读 ObjectRegistry。
- ObjectRegistry 的插入、替换和删除入口保持私有，只开放按稳定 ID 查询和确定性枚举。调用方不能取得活动对象的可变引用、裸指针或位置索引。
- `DocumentStore::addDocument` 只是装载/组合生命周期入口，只创建 revision 0 的空文档，不是业务内容写入口。

任何文档对象写入必须进入 `ApplicationTransaction`。当前没有绕过事务直接修改活动 Document 或 ObjectRegistry 的公共 API。

## 修订模型

RevisionSet 固定覆盖六类一致性范围：Project、Document、Geometry、CAM、MachineContext、Environment。

- 对象创建、更新或删除自动影响 ProjectRevision 和 DocumentRevision；
- 事务可用 `touchRevision` 声明 Geometry、CAM、MachineContext 或 Environment 等额外受影响范围；
- `TransactionManager::begin` 可校验调用方显式提供的 Revision Preconditions；
- commit 会再次校验事务开始时捕获的全部六类 Revision，而不只检查调用方显式前置条件；
- 同一项目的 ProjectRevision 在各文档间共享，因此任一文档提交都会使该项目内较早快照失效。这是当前阶段有意采用的保守冲突粒度；
- Revision 只允许单调递增，重复前置条件、非法 scope 和 `uint64_t` 溢出均 fail-closed；推进集合中的重复 scope 按一次递增处理，计算下一组 Revision 失败时不会改变活动状态。

后台任务在后续阶段只能基于不可变 Document 快照计算，最终结果必须重新进入 ApplicationTransaction；旧 Revision 的结果不得覆盖新状态。

## 应用事务

事务状态为 Active、Failed、Committed、RolledBack。标准路径为：

`Begin -> Modify Staging State -> Collect Pending Events -> Validate Revisions -> Build Commit -> Atomic Swap -> Return Committed Events`

具体约束如下：

1. begin 捕获完整 Document 快照，并为稳定 TransactionId 建立活动占用；重复活动 ID 被拒绝。
2. 对象修改只作用于事务私有的 copy-on-write staging registry，commit 前外部快照不可见。
3. 任一修改失败都会使事务进入 Failed；后续操作返回 `Transaction.Failed`，并保留首个失败作为 cause。失败事务不能部分提交。
4. 同一事务内删除已有对象后，不允许用相同稳定 ObjectId 创建另一个对象，避免身份重生。
5. commit 先构造确定性的 before/after change set、下一组 Revision、CommittedDomainEvent 和回执；所有可能失败的准备完成后，才在 DocumentStore 独占锁内用无抛出的 swap 原子替换对象状态并更新 Revision。
6. 空净变更不能推进 Revision，也不能释放事件；rollback、析构放弃、并发冲突、文档消失和 Revision 溢出均不得改变活动 Document。
7. 并发事务提交在 DocumentStore 内串行化；基于同一旧快照的竞争提交只能有一个成功，其余返回 Revision Conflict。
8. `TransactionManager` 与 `DocumentStore` 必须比活动事务存活更久；调用方必须在销毁 AppKernel 前提交、回滚或释放全部事务，显式 shutdown 会在仍有活动事务时拒绝执行。AppKernel 生命周期与单个 `ApplicationTransaction` 均只允许由一个调用线程驱动。

Application Transaction 与 SQLite transaction 不是同一概念。前者负责内存业务状态、修订、变更材料和领域事件一致性；后者目前只是一项底层持久化原语。

## 提交后事件规则

- commit 前事件只能以 `PendingDomainEvent` 存在于事务私有集合中；
- `CommittedDomainEvent` 不是公开可构造的数据结构，只能由 TransactionManager 在成功提交路径创建；
- 成功回执中的事件携带 TransactionId、ProjectId、DocumentId、提交后 RevisionSet 和事务内稳定 sequence；
- 事件顺序与收集顺序一致；提交失败或空提交不产生可发布事件；
- Phase 4 不实现 EventBus，也不在 DocumentStore 锁内调用回调。Phase 5 只能发布成功 `TransactionCommit` 回执中的事件。

## 变更材料与阶段边界

`TransactionCommit::changes` 对 Created、Updated、Removed 保存稳定 ObjectId 以及 before/after ObjectRecord。这是后续 Undo、Journal 和持久化的输入材料，不代表这些能力已经实现。

明确不属于 Phase 4：

- CommandRuntime、QueryRuntime、EventBus 与 Headless/CLI 入口，留给 Phase 5；
- Undo/Redo 管理器、Snapshot、Journal、SQLite Persistence 编排和 Crash Recovery，留给 Phase 8；
- TaskRuntime、Scheduler、Workflow、Script、RPC、AI；
- CAD、CAM、Machine、Collision、Process、Qt GUI 或任何上层模块。

## 主要错误契约

- `Project.RevisionConflict`：显式前置条件或 commit 捕获修订已过期；
- `Revision.InvalidScope`、`Revision.DuplicatePrecondition`、`Revision.Overflow`：修订输入或推进失败；
- `Transaction.AlreadyActive`、`Transaction.NotActive`、`Transaction.Failed`：事务状态不允许当前操作；
- `Transaction.EmptyCommitDenied`：没有文档净变更；
- `Document.ObjectAlreadyExists`、`Document.ObjectNotFound`、`Document.ObjectIdReuseDenied`：对象身份规则失败；
- `Kernel.ActiveTransactions`：AppKernel 仍有活动事务，拒绝 shutdown。
