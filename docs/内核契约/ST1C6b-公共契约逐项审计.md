# ST1C6b 公共契约逐项审计

## 状态与判定方法

起点 `7377f14`，承接 [C6 总计划](ST1C6-公共契约与输入预算.md) 和 [71 个公共头差异基线](Kernel-1.0-公共头清单.md)。本表区分“声明已登记”“行为已有证据”“缺口待实现”；不以文件数量或源码阅读替代逐入口负例、预算和最终签核。

本轮先登记 Infrastructure 的 8 个公开类、5 个 Options 的全部 13 个字段，以及对应 6 个 platform 端口。路径编码的真实缺陷作为 C6b1 修复；日志别名仍待独立验证和实现。Foundation、Kernel/Host、Runtime 执行 DTO、State、消息观察与 Persistence 的其他字段/版本审计尚未完成，不宣称 C6b 已收口。

## Infrastructure 的配置字段

下列默认值和入口以当前公开声明为准。构造独立基础设施组件属于 Host 装配能力，不代表获得 AppKernel 运行中业务写权限，也不替代 ExecutionGateway、Command/Transaction、恢复认证和活动租约。

| 所属 Options | 字段与默认值 | 当前准入及错误边界 |
| --- | --- | --- |
| [SqliteConnectionOptions](../../include/lasercnc/infrastructure/sqlite_persistence_backend.hpp) | databasePath：path，默认空 | open 拒绝空、NUL、非法 UTF-16，Persistence.InvalidOptions；精确 `:memory:` 仍可用；其他原生路径转换为 UTF-8 后交给 SQLite |
| 同上 | busyTimeoutMilliseconds：int，5000 | 负数 InvalidOptions；非负值交给 sqlite3_busy_timeout，不是整个调用的可抢占 deadline |
| 同上 | enableForeignKeys：bool，true | open 设置连接 PRAGMA；持久 Host 的 acquireHostSession 另强制并读回策略，不能用 false 绕过 Host 会话认证 |
| [FilesystemSnapshotStoreOptions](../../include/lasercnc/infrastructure/filesystem_snapshot_store.hpp) | directory：path，默认空 | create 拒绝空、NUL、非法 UTF-16、末尾点/空格目录组件；前者为 Snapshot.InvalidStoreOptions；原生长路径不改变逻辑 SnapshotId |
| 同上 | maximumPayloadBytes：size_t，64 MiB | 正数且留出信封最大开销；只约束单个逻辑负载，不含外层身份信封，不是总磁盘/总历史预算 |
| [FilesystemAssetStoreOptions](../../include/lasercnc/infrastructure/filesystem_asset_store.hpp) | directory：path，默认空 | 委托 Snapshot 准入；路径拒绝为 Asset.StoreInitializationFailed → Snapshot.InvalidStoreOptions |
| 同上 | maximumContentBytes：size_t，512 MiB | 正数且保留资产信封空间；还须满足底层 Snapshot 的外层开销；缺 hash service 或本层非法大小为 Asset.InvalidStoreOptions |
| [SpdlogLogOptions](../../include/lasercnc/infrastructure/spdlog_log_service.hpp) | enableConsole：bool，true | 至少启用一个输出，否则 Logging.InvalidOptions；不是日志可靠持久化确认 |
| 同上 | rotatingFilePath：optional&lt;path&gt;，未设置 | 已设置时必须非空、无 NUL、UTF-16 完整；与 JSONL 一起完成参数预检，才构造 sink |
| 同上 | jsonlFilePath：optional&lt;path&gt;，未设置 | 同上；当前双路径仅 lexically_normal 比较，Windows 别名和轮转目标关系待补查，不能视为全物理身份保证 |
| 同上 | rotatingFileMaxBytes：size_t，5 MiB | 有文件输出时必须非零；轮转阈值不是单条 LogRecord/结构化数据的输入预算，超大单条记录另归 C6c |
| 同上 | rotatingFileCount：size_t，3 | 有文件输出时必须非零；更大的库级上限当前由 spdlog 构造抛错、映射 InitializeFailed；内核统一上限/错误分类仍待预算审计 |
| [BsThreadPoolExecutorOptions](../../include/lasercnc/infrastructure/bs_thread_pool_executor.hpp) | threadCount：size_t，0 | 传给锁定线程池的默认选择；不是 Task 队列/历史保留上限，具体容量与资源失败契约由 C6c/d、C7 验证 |

## 公开操作与端口对应

以下枚举操作面，不重复展开每个类的析构和删除的复制/移动声明。工厂成功交回 unique_ptr；文件/线程池实现持有私有 Impl，hash 服务通过 shared_ptr 持有。销毁期间必须满足所属对象的使用寿命和并发结束约束，不能从“持有智能指针”推导所有并发析构都安全。

| Adapter / 端口 | 公开操作与返回 | 已知边界与未闭合项 |
| --- | --- | --- |
| SqlitePersistenceBackend / [IPersistenceBackend](../../include/lasercnc/platform/persistence_backend.hpp) | open；acquireHostSession → Result&lt;PersistenceSessionInfo&gt;；execute(statement, parameters) → Result&lt;size_t&gt;；query → Result&lt;vector&lt;PersistenceRow&gt;&gt;；beginTransaction/commitTransaction/rollbackTransaction → Result&lt;void&gt; | 原始 SQL 端口是装配/持久组件能力，不是 Host 业务入口；会话独占持续到后端销毁。SQL、参数、行集累计预算及异常分配边界仍须 C6c 审查 |
| FilesystemSnapshotStore / [ISnapshotStore](../../include/lasercnc/platform/snapshot_store.hpp) | create；writeAtomically(SnapshotId, string_view) → Result&lt;SnapshotWriteDisposition&gt;；read → Result&lt;string&gt;；remove → Result&lt;bool&gt; | 精确身份、不可变负载、Created/AlreadyPresent、同句柄删除见 C3；文件读取不等于应用恢复授权。目录编码新拒绝不改外层 LCNCSN02 |
| FilesystemAssetStore / [IAssetStore](../../include/lasercnc/platform/asset_store.hpp) | create(options, hashes)；publish(AssetKind, span&lt;byte&gt;) → Result&lt;AssetRef&gt;；read → Result&lt;vector&lt;byte&gt;&gt;；verify → Result&lt;void&gt; | 身份绑定 kind/digest/size，校验信封及内容摘要；不新增资产删除/回收入口。资产信封 LCNCAS01、身份派生 LCNCAssetRef1 与 Snapshot 外层版本分别登记，不混成一个版本 |
| SpdlogLogService / [ILogService](../../include/lasercnc/observability/log_service.hpp) | create；write(LogRecord)；flush，后两者返回 Result&lt;void&gt; | 文件名为 Windows 宽字符、内容为 UTF-8。write 会序列化结构化记录；失败为 SerializationFailed/WriteFailed，flush 为 FlushFailed；不能据此承诺文件系统事务或物理落盘 |
| [JsonconsAdapter](../../include/lasercnc/infrastructure/jsoncons_adapter.hpp) / IValueSerializer、ISchemaValidator | serialize(Value) → Result&lt;string&gt;；deserialize(string_view) → Result&lt;Value&gt;；validate(Schema, Value) → Result&lt;void&gt; | JsonEncodeFailed/JsonParseFailed/SchemaBackendFailed 与 Runtime.SchemaInvalid 分开；递归转换、Schema 编译及输入/输出预算尚未统一，不能仅依赖库默认值 |
| [TomlConfigAdapter](../../include/lasercnc/infrastructure/toml_config_adapter.hpp) / [IConfigSerializer](../../include/lasercnc/platform/config_serializer.hpp) | parse(content, sourceName) → Result&lt;Value&gt;；serialize(root) → Result&lt;string&gt; | sourceName 是诊断来源而非文件读取请求；序列化根必须 Object，TOML null/date-time 不属于当前双向 Value 子集。RootNotObject 与 ParseFailed/SerializeFailed 区分，深度和总量待 C6c |
| [Sha256HashService](../../include/lasercnc/infrastructure/sha256_hash_service.hpp) / [IHashService](../../include/lasercnc/platform/hash_service.hpp) | digest(span&lt;const byte&gt;) → Result&lt;ContentDigest&gt; | Windows CNG 的 sha256: 小写十六进制；ProviderFailed 带 operation/provider/status。分块调用库不等于整条请求有取消/耗时或总量预算 |
| BsThreadPoolExecutor / [ITaskExecutor](../../include/lasercnc/platform/task_executor.hpp) | create；submit(work, completion)、waitIdle、shutdown → Result&lt;void&gt;；drainForDestruction → noexcept void；isCurrentWorkerThread → noexcept bool；concurrency → noexcept size_t | submit 成功要求恰好一次 completion，可同步；失败/异常不得保留工作。shutdown 失败不算确认；drain 是可无期限等待的最终屏障，不能从所属 worker 销毁。无强制终止 handler 的承诺，任务/终态预算待 C6d |

platform 的具名 DTO/别名：PersistenceRow = Value::Object；PersistenceSessionInfo 的 backend:string、persistent:bool=false、configuration:Value；SnapshotWriteDisposition 底层 uint8_t，枚举 Created/AlreadyPresent；ExecutorWork = function&lt;Result&lt;void&gt;()&gt;，ExecutorCompletion = function&lt;void(Result&lt;void&gt;)&gt;。这些类型的完整嵌套 Value、Error、AssetRef、StrongId 与恢复 wire 字段仍须跨族审计，尚未签核。

## C6b1 路径编码与错误契约

五个路径选项的输入域统一为非空、无内嵌 NUL、UTF-16 代理对完整的 Windows 原生路径。拒绝不配对代理码元是内核准入规则；不宣称 NTFS 无法保存这种原生名称，也不以替代字符、ANSI 编码或截短路径继续执行。此前可被底层接纳的原生异常名称现在返回验证拒绝，属于行为兼容收紧；合法 BMP 与补充平面名称保留。

SQLite 的失败为 Validation/Persistence.InvalidOptions；Snapshot 为 Validation/Snapshot.InvalidStoreOptions；Asset 保持 Infrastructure/Asset.StoreInitializationFailed 并携带前者 cause；日志为 Validation/Logging.InvalidOptions。未配对路径的 Snapshot 诊断明确省略 path，提供 pathEncoding="invalid-utf16"、pathOmitted=true，防止错误构造再次抛出转换异常；正常路径仍有原 path。不宣称 Error.details 永远含 path，消费者应按字段存在性读取。

这是对已知编码输入的 Result 保证，不是所有工厂或任意内存耗尽情况下的 noexcept 保证；参数构造、内存分配和其他系统异常仍须逐入口核对。错误 DTO 顶层形状、所有公共头、SQLite 迁移和 Journal/Snapshot/Asset 版本本轮不变；详细证据见 [C6b1 交付](../阶段交付/2026-09-04-ST1C6b1-路径编码准入与契约登记.md)。

## 后续顺序（保留完整目标）

1. 日志双输出已有/未创建路径、大小写、硬链接、父目录别名和轮转文件族，先独立证明实际冲突与保留内容，再决定身份准入实现；不得仅 case-fold 后拒绝所有合法大小写敏感目录来缩小目标。
2. 继续 Foundation/Host/执行/状态/观察/持久族逐类型、枚举和 DTO 字段；将源码兼容、权限阶段、线程/寿命、Error cause 和 wire 版本分别关联到测试。
3. C6c/d 执行统一预算、同步与 Task、终态保留；C7 测容量，C8 完整日志/脚本/私有头门禁，ST1D 最终三配置签核。上述均未被本轮 8 个 Adapter 声明登记替代。
