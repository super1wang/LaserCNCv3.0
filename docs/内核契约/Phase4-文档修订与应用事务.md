# Phase 4 文档、修订与应用事务

状态：进行中。

Phase 4 建立 Kernel 自有的业务状态与一致性边界，不引入领域对象、GUI、CommandRuntime、EventBus、持久化策略或第三方框架。

## 当前已建立的状态基座

- `ProjectId`、`DocumentId`、`ObjectId`、`ObjectTypeId`、`TransactionId` 均为独立 StrongId，不能互相混用。
- `DocumentStore` 统一拥有文档可变状态和跨文档共享的 ProjectRevision。
- 对外只返回按值复制的 `Document` 不可变快照；快照包含稳定身份、RevisionSet 和只读 ObjectRegistry。
- ObjectRegistry 的插入、替换和删除入口保持私有，只开放稳定 ID 查询和确定性枚举。
- RevisionSet 覆盖 Project、Document、Geometry、CAM、MachineContext、Environment 六类一致性范围。
- RevisionManager 负责前置条件校验和溢出保护；冲突统一返回 `Project.RevisionConflict`。

`DocumentStore::addDocument` 是装载/组合生命周期入口，只创建 revision 0 的空文档，不是业务内容写入口。文档内容修改必须由本阶段后续的 ApplicationTransaction 完成。

## 待完成的同阶段契约

- ApplicationTransaction 采用隔离 staging state，任一步失败后不得提交部分状态；
- commit 必须再次校验捕获的 ProjectRevision 与 DocumentRevision；
- 成功 commit 原子替换文档状态并单调递增相关 Revision；
- Object change set 同时保存 before/after，作为后续 Undo/Journal 的稳定材料；
- Domain Event 在 commit 前只属于事务私有集合，只有成功 commit 的回执可以交给 Phase 5 EventBus；
- rollback、析构放弃、并发冲突和 Revision 溢出均不得改变活动 Document。
