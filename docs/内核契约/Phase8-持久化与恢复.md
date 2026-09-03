# Phase 8 持久化与恢复契约

## 阶段状态

Phase 8 正在建设。目标是以 Snapshot、状态 Journal、SQLite Control Plane 和原子文件 Data Plane 建立可校验、可重放、可故障注入的 Crash Recovery。本阶段只处理 Application Kernel 持久化语义，不实现 Workflow、Script、CAD/CAM、GUI、设备控制或其他上层模块。

当前已完成强内容摘要端口、Windows Adapter、PersistenceService v5 schema migration、状态 Journal、ApplicationTransaction 写前接入、Snapshot 文件/索引、启动恢复重放、持久 Command 幂等、Task History 和 Diagnostics Metadata。完整多配置、重复、Production-only 与独立进程门禁仍未完成，因此本阶段尚未验收。

## 校验和身份

- SnapshotId 是稳定 StrongId；ContentDigest 是带算法前缀的稳定强摘要身份。
- Kernel 公共 API 只定义 IHashService：输入只读字节序列，输出 ContentDigest；不得暴露算法上下文或平台句柄。
- 当前 Windows Adapter 使用操作系统 CNG 的 SHA-256，规范文本为 `sha256:<64 位小写十六进制>`。
- 哈希实现可替换，但已落盘记录的算法前缀不可省略；恢复必须按记录算法验证，不能把普通 HashMap hash 当作持久校验和。
- 哈希提供者失败统一转换为 `Hash.ProviderFailed`，不得生成空摘要或继续写入未校验记录。

## Snapshot 实现契约

- Snapshot 保存某一 Document 在明确 RevisionSet 下的完整内核状态，不保存运行时指针、第三方对象或上层大型资产本体。
- Snapshot payload 进入原子文件 Data Plane；SQLite `snapshot_index` 只保存 SnapshotId、ProjectId、DocumentId、全局 Journal 水位、Revision、存储键、ContentDigest、payload 大小和创建时间。
- 写入顺序必须先产生临时文件并 flush，再原子替换最终文件，最后提交 SQLite 索引；失败不得留下“索引已成功但文件不存在”的可用记录。
- 恢复只选择状态完整且摘要验证通过的最新 Snapshot；损坏、缺失、格式不兼容或 Revision 元数据不一致时 fail-closed，不能静默降级到不可信状态。
- `FilesystemSnapshotStore` 只接受由字母、数字、点、下划线和连字符组成且不超过 128 字节的 SnapshotId 文件名；拒绝路径穿越和超限 payload。
- SnapshotId 是不可变内容身份：相同 ID/相同内容重试返回 AlreadyPresent，相同 ID/不同内容返回 `Snapshot.IdentityConflict`。
- Windows 文件 Adapter 使用 `CreateFileW -> WriteFile -> FlushFileBuffers -> MoveFileExW(MOVEFILE_WRITE_THROUGH)` 发布；临时文件失败时清理，最终文件不会被覆盖。
- `captureSnapshot()` 在 SQLite `BEGIN IMMEDIATE` 内确认 Document 局部修订与最新本 Document Journal 一致、ProjectRevision 与最新本项目 Journal 一致，并使用数据库当前全局最大 sequence 作为水位。这样同一项目多文档交错提交时，未刚好写入本 Document 的 ProjectRevision 也不会被误判。
- Data Plane 文件必须先成功发布，再插入并提交 Control Plane 索引。数据库失败可能留下不可达孤儿文件，但不会留下指向缺失文件的已提交索引；相同 SnapshotId 可安全重试收敛。

## Journal 目标契约

- Journal 输入只能来自成功 ApplicationTransaction 的 TransactionCommit，保存稳定 TransactionId、ProjectId、DocumentId、修订前后值、ObjectChange 和已提交 Domain Event 材料。
- Journal append 必须在内存 Document 替换前完成持久提交；持久提交成功后的内存 swap/revision 替换必须保持无失败路径，使崩溃点最多形成“Journal 已落盘、内存尚未更新”，下次恢复可以重放。
- Journal sequence 单调且唯一；同一 TransactionId 重试不得产生不同内容，内容相同可返回原记录，内容冲突必须 fail-closed。
- 状态恢复只应用 ObjectChange 与 Revision，不主动重新发布历史 Event，也不重放 Command handler 或任何外部副作用。

## 当前 Journal 实现

- PersistenceService 组合期接收唯一的 IPersistenceBackend、IValueSerializer 与 IHashService；缺失端口、重复配置、未初始化使用和 Ready 后变更均返回明确 Error。
- `initialize()` 在底层数据库事务内建立 v5 schema：`schema_migrations`、`state_journal`、`snapshot_index`、`command_idempotency`、`task_history`、`diagnostic_history` 及所需索引；数据库版本高于内核支持版本时返回 `Persistence.SchemaTooNew`。
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

## 持久 Command 幂等

- CommandRuntime 在 schema 校验和 capability 授权后，以 Command 名称/版本、参数、Session、Project、Document 和期望 ProjectRevision 构造版本化请求签名。
- `command_idempotency` 保存 key、签名 payload/digest、状态和完成结果。相同 key 绑定不同签名返回冲突；签名或结果摘要损坏时 fail-closed。
- 同步 Command 的 Journal append 与幂等结果完成处于同一 SQLite 事务；不存在“业务状态已持久但幂等结果缺失”的正常提交窗口。
- 异步 Command 先在 Scheduler 建立不可调度的预备记录，再以同一 SQLite 事务写入 Task 接受记录和 Command 接受结果，提交后才激活 Task。
- 重启后的成功重试直接返回原 TransactionCommit 或 TaskId，不重新调用 Command handler、不重复发布历史 Event，也不重复提交 Task。
- 启动时仅把上次进程遗留的 `pending` claim 标记为 `abandoned`，允许同签名重试重新取得；该恢复语义要求一个数据库在同一时刻只由一个活动 AppKernel Host 拥有，多进程共享写入不在本阶段支持范围内。

## Task History

- `task_history` 在 Task 可调度前保存版本化请求、输入、依赖、资源声明、稳定身份、上下文、修订条件、捕获的 source revisions 和 deadline 身份材料，并计算强摘要。
- Task 的 Succeeded、Failed、Cancelled、Stale 终态保存完整结果或 Error 材料；终态不可变，相同内容可幂等重试，不同终态或结果返回 `Persistence.TaskOutcomeConflict`。
- Scheduler 在运行完成、执行前超时、依赖失败、资源获取失败、取消和关闭取消路径记录终态；`wait()` 只在终态持久化已成功或失败已进入有界错误缓存后返回。
- 终态持久化失败不反转已完成的 Task 结果，失败可经 `Scheduler::persistenceFailures()` 检索；下次启动仍会把无持久终态的 accepted/running 记录标记为 `Task.InterruptedByRestart`。
- 重启后 `TaskRuntime::snapshot()`/`wait()` 可在内存 Scheduler 不含该 TaskId 时读取持久历史，但不会自动重跑未完成任务或恢复 handler 执行栈。

## Diagnostics Metadata

- DiagnosticsService 新增组合期 `IDiagnosticExporter`，与 Trace/Metrics 一样先更新内存 latest，再在内部锁外调用 exporter；失败只进入有界 `exporterFailures()`。
- AppKernel 挂接 PersistenceService 代理：未配置持久化时为纯内存 no-op；配置后每次报告追加到 `diagnostic_history`。
- 诊断 payload 保存稳定 DiagnosticId、状态、摘要、details 和观察时间，并以 SHA-256 校验；`diagnosticHistory()` 返回单项顺序历史，`latestDiagnostics()` 按稳定 ID 返回每项最新报告。
- 任一历史 payload 摘要或控制面元数据不一致时读取 fail-closed。SQLite 写失败不改变已经产生的 DiagnosticReport，也不触发自动恢复动作。

## Crash Recovery 实现契约

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

当前 `recover()` 在一致的 SQLite 读事务中取得全部 Journal 与 Snapshot 索引，然后：

- 校验全局 Journal sequence 必须从 1 严格连续，按项目验证 ProjectRevision 链，按文档验证其余五域 Revision 链；
- 每个文档只选取水位最高的 Snapshot，验证文件大小、SHA-256、格式版本、稳定身份、Revision 和 Journal 水位；最新记录损坏时直接失败，不回退到旧快照；
- 没有 Snapshot 的文档从空状态重放；已有 Snapshot 的文档只应用其水位后的 ObjectChange；Created/Updated/Removed 均核对对象是否存在及 before 状态；
- 同项目不同文档即使具有不同 Snapshot 水位，也沿全局 Journal 顺序推进共享 ProjectRevision，最终恢复为同一项目修订；
- 输出纯内核 `DocumentImage`，再由 AppKernel 在模块启动和 Runtime Ready 前原子装入 DocumentStore；装入失败或配置时项目归属冲突时不进入 Ready；
- Journal 中保存的 Domain Event 只验证其容器格式，不投递到 EventBus；恢复过程不调用任何业务 handler。

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
- 原子 Snapshot 文件的首次写入、幂等重试、身份冲突、路径穿越、大小限制、读取、删除与重开读取；
- v2 `snapshot_index` migration、Snapshot 与 Journal/ProjectRevision 对齐、同项目多文档全局水位；
- Snapshot + Journal tail 恢复、纯 Journal 恢复、DocumentStore 原子装载和 AppKernel Ready 前恢复；
- 恢复不重新发布历史 Domain Event；
- Journal sequence 缺口、对象 before 状态冲突、Snapshot 缺失/截断/摘要损坏均 fail-closed。
- 同步/异步 Command 的持久幂等、key 重绑冲突、原 Commit/TaskId 重启重放，以及重放不调用 handler/不重发 Event；
- Task 原子接受、成功终态、不可变结果、重启中断标记、请求/终态摘要篡改闭锁和终态写失败可见性；
- Diagnostics 内存优先 exporter、SQLite 顺序历史/latest、重启读取、摘要篡改闭锁和写失败隔离。
- 独立播种进程以 `_Exit` 跳过析构后，恢复进程可验证 Snapshot + Journal tail、原 Commit 幂等结果、未完成 Task 中断和 Diagnostics 历史，且 handler/Event 不重放。

## 尚未验收

- 数据库 busy、只读文件系统和实际进程中断点等更完整故障注入；
- Release、Debug 重复、Production-only 与最终架构扫描门禁。
