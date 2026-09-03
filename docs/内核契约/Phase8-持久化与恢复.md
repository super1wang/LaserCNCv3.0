# Phase 8 持久化与恢复契约

## 阶段状态

Phase 8 正在建设。目标是以 Snapshot、状态 Journal、SQLite Control Plane 和原子文件 Data Plane 建立可校验、可重放、可故障注入的 Crash Recovery。本阶段只处理 Application Kernel 持久化语义，不实现 Workflow、Script、CAD/CAM、GUI、设备控制或其他上层模块。

当前已完成强内容摘要端口、Windows Adapter、PersistenceService v1 schema migration、状态 Journal 和 ApplicationTransaction 写前接入。Snapshot 文件、恢复重放和其他元数据仍未完成；下述未完成条目是后续实现必须满足的准入约束。

## 校验和身份

- SnapshotId 是稳定 StrongId；ContentDigest 是带算法前缀的稳定强摘要身份。
- Kernel 公共 API 只定义 IHashService：输入只读字节序列，输出 ContentDigest；不得暴露算法上下文或平台句柄。
- 当前 Windows Adapter 使用操作系统 CNG 的 SHA-256，规范文本为 `sha256:<64 位小写十六进制>`。
- 哈希实现可替换，但已落盘记录的算法前缀不可省略；恢复必须按记录算法验证，不能把普通 HashMap hash 当作持久校验和。
- 哈希提供者失败统一转换为 `Hash.ProviderFailed`，不得生成空摘要或继续写入未校验记录。

## Snapshot 目标契约

- Snapshot 保存某一 Document 在明确 RevisionSet 下的完整内核状态，不保存运行时指针、第三方对象或上层大型资产本体。
- Snapshot payload 进入原子文件 Data Plane；SQLite 只保存 SnapshotId、ProjectId、DocumentId、Revision、相对路径、ContentDigest、格式版本和创建状态。
- 写入顺序必须先产生临时文件并 flush，再原子替换最终文件，最后提交 SQLite 索引；失败不得留下“索引已成功但文件不存在”的可用记录。
- 恢复只选择状态完整且摘要验证通过的最新 Snapshot；损坏、缺失、格式不兼容或 Revision 元数据不一致时 fail-closed，不能静默降级到不可信状态。

## Journal 目标契约

- Journal 输入只能来自成功 ApplicationTransaction 的 TransactionCommit，保存稳定 TransactionId、ProjectId、DocumentId、修订前后值、ObjectChange 和已提交 Domain Event 材料。
- Journal append 必须在内存 Document 替换前完成持久提交；持久提交成功后的内存 swap/revision 替换必须保持无失败路径，使崩溃点最多形成“Journal 已落盘、内存尚未更新”，下次恢复可以重放。
- Journal sequence 单调且唯一；同一 TransactionId 重试不得产生不同内容，内容相同可返回原记录，内容冲突必须 fail-closed。
- 状态恢复只应用 ObjectChange 与 Revision，不主动重新发布历史 Event，也不重放 Command handler 或任何外部副作用。

## 当前 Journal 实现

- PersistenceService 组合期接收唯一的 IPersistenceBackend、IValueSerializer 与 IHashService；缺失端口、重复配置、未初始化使用和 Ready 后变更均返回明确 Error。
- `initialize()` 在底层数据库事务内建立 `schema_migrations`、`state_journal` 和 `(document_id, sequence)` 索引；数据库版本高于内核支持版本时返回 `Persistence.SchemaTooNew`。
- v1 payload 保存格式名/版本、Transaction/Project/Document ID、六域修订前后值、完整 ObjectChange before/after 和已提交 Domain Event 材料。
- Revision 以十进制字符串保存，避免 Kernel 的 uint64 Revision 被 Value/SQLite int64 截断；Journal sequence 使用 SQLite 正整数范围。
- append 在 BEGIN IMMEDIATE 内查询 TransactionId：相同 payload/digest 返回原 sequence，不同内容返回 `Persistence.JournalTransactionConflict`；新记录写入后回读，再提交数据库事务。
- `journalAfter()` 按 sequence 读取，逐条重新计算摘要、反序列化 payload，并核对格式、版本、身份和 Revision 元数据。摘要或元数据不一致立即 fail-closed。
- Adapter 返回失败和异常路径都会尝试回滚；回滚本身失败使用 `Persistence.RollbackFailed` 保留主失败因果和回滚错误材料。

## ApplicationTransaction 接入

TransactionManager 使用独立 commit mutex 串行化所有文档提交，避免在数据库 I/O 期间长期占用 DocumentStore 锁：

```text
commit mutex
  -> Document 写锁：校验当前六域 Revision，准备 TransactionCommit
  -> 释放 Document 写锁
  -> PersistenceService.append（serializer/hash/SQLite）
  -> Document 写锁：ObjectRegistry swap + Revision 替换（无失败路径）
  -> 释放 commit mutex
```

因此 serializer/hash/backend 不在 DocumentStore 锁内调用，Query/诊断可以继续读取旧快照；其他事务提交由 commit mutex 等待，不能越过当前 Journal sequence。Journal 失败时 staging state 被回滚，内存对象和 Revision 不变，CommandRuntime 也不会发布尚未提交的 Event。

AppKernel 拥有 PersistenceService：配置时在 bootstrap 执行 migration，失败则不能进入 Ready；Ready 边界冻结配置。未配置 PersistenceService 时保留显式的纯内存运行模式，便于单元测试和无持久化 Host，但不能宣称具备崩溃恢复。

## Crash Recovery 目标契约

```text
打开 kernel.db / 执行 schema migration
  -> 枚举项目与文档
  -> 读取最新完整 Snapshot 索引
  -> 原子读取 payload / 验证 ContentDigest
  -> 重建不可变 Document 状态
  -> 按 sequence 重放 Snapshot 之后的 Journal
  -> 每步校验 revisionsBefore / revisionsAfter
  -> Ready
```

任一摘要、序列、Revision、对象 before/after 或格式版本不匹配时，恢复返回明确 Error，AppKernel 不得进入 Ready。恢复不调用 Command/Query/Task handler，不发布历史事件，也不执行文件、控制器、运动或激光副作用。

## SQLite 与文件边界

- SQLite 是 Control Plane：schema_migrations、project/document metadata、state journal、snapshot index、idempotency、task/diagnostics metadata。
- 文件系统是 Data Plane：Snapshot payload 以及未来几何、网格、刀路等大型资产。
- 当前 SqlitePersistenceBackend 仍是低层参数化单语句/事务原语；只有 Phase 8 PersistenceService 可以编排表结构和持久化事务，领域模块不得散布 SQL。
- 通用 Value 通道继续拒绝 SQLite BLOB；复合 Value 必须先通过 IValueSerializer 形成版本化文本 payload，再计算 ContentDigest。

## 当前已验收

- SnapshotId、ContentDigest 与 IHashService 公共类型无第三方/Windows 类型泄漏；
- Windows CNG SHA-256 对空内容和 `abc` 标准向量产生已知摘要；
- 相同内容摘要稳定，不同内容摘要不同；
- CNG 状态在 Adapter 边界转换为统一 Error。
- v1 migration 幂等执行，并拒绝更高未知 schema 版本；
- Journal 首次追加、相同 TransactionId/内容重试、冲突重绑、严格顺序、过滤读取和数据库重开；
- payload SHA-256 篡改检测，以及 payload/控制面身份与 Revision 一致性检查；
- migration Adapter 抛异常后的数据库回滚；
- TransactionManager 在 serializer 重入读取 Document 时无死锁，且读取到提交前快照；
- Journal 成功后更新内存，Journal 失败时对象和 Revision 均不改变；
- AppKernel 对 PersistenceService 的拥有与 Ready 冻结。

## 尚未验收

- Snapshot codec、原子文件 store、SQLite snapshot index；
- DocumentStore 恢复装载和 Journal 重放；
- 持久幂等、Task/Diagnostics metadata；
- 校验和损坏、截断、断电点、数据库忙/只读/提交失败等故障注入；
- Debug/Release、重复、Production-only 与完整进程恢复门禁。
