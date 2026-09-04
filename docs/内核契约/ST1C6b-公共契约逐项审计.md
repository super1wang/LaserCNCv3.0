# ST1C6b 公共契约逐项审计

## 状态与判定方法

起点 `7377f14`，承接 [C6 总计划](ST1C6-公共契约与输入预算.md) 和 [71 个公共头差异基线](Kernel-1.0-公共头清单.md)。本表区分“声明已登记”“行为已有证据”“缺口待实现”；不以文件数量或源码阅读替代逐入口负例、预算和最终签核。

本轮先登记 Infrastructure 的 8 个公开类、5 个 Options 的全部 13 个字段，以及对应 6 个 platform 端口。路径编码的真实缺陷作为 C6b1 修复；日志别名按 [C6b2 契约](ST1C6b2-日志文件身份与轮转准入.md) 补回归并实现。继续登记 Foundation 的 7 个头、Observability 的 5 个头和 Messaging 的 2 个头所含声明与待验证点；这些登记不等于行为签核。C6b17 已补 Journal/Task/Workflow 持久写入的枚举与关键形状，Kernel/Host、State、Task Error cause 及 Persistence 的其他字段/版本审计仍未完成，不宣称 C6b 已收口。

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
| 同上 | jsonlFilePath：optional&lt;path&gt;，未设置 | 同上；C6b2 加入目录身份/大小写/轮转命名及现有物理文件身份预检；稳定可信文件根以外的运行期替换/占用仍不属于静态准入保证 |
| 同上 | rotatingFileMaxBytes：size_t，5 MiB | 有文件输出时必须非零；轮转阈值不是单条 LogRecord/结构化数据的输入预算，超大单条记录另归 C6c |
| 同上 | rotatingFileCount：size_t，3 | 有文件输出时必须非零；C6b2 将超过锁定库 MaxFiles=200000 的错误提前归为 InvalidOptions，并预检所有轮转目标。该最大数值未获容量认证，实际资源预算/成本仍待 C6c/C7 |
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

## Foundation 声明与后续验证索引

下表依据 7 个公共头及其实际实现登记，不修改基础类型，也不把阅读结果计入本轮日志测试成绩。相关旧用例入口为 tests/unit/foundation 的 value_tests、schema_tests、strong_id_tests、result_error_tests；它们尚不覆盖表中全部异常/资源边界。

| 文件与声明 | 字段、值域及公开操作 | 已知边界与必须补查 |
| --- | --- | --- |
| [value.hpp](../../include/lasercnc/foundation/value.hpp)：Value | Array=vector&lt;Value&gt;，Object=map&lt;string,Value,less&lt;&gt;&gt;；Storage 依次为 nullptr/bool/int64/double/string/Array/Object；Kind:uint8_t 对应七种。九个构造形式、kind/storage、const/可变 getIf&lt;T&gt;、默认相等比较 | 默认与 null char* 构造为 Null；string/容器为值所有权，可通过 getIf 修改内容。当前不检查非有限 double、编码或递归总量，不能把值载体当作已经通过入口预算的材料 |
| [error.hpp](../../include/lasercnc/foundation/error.hpp)：ErrorCode、Error、makeError | ErrorCode 显式 string 构造、value 与相等；Error 的 code/category=Internal/severity=Error/message/details/cause；cause 为 shared_ptr&lt;const Error&gt;。ErrorCategory:uint8_t 为 Validation/NotFound/Conflict/Authorization/Cancellation/Timeout/Infrastructure/Internal；Severity:uint8_t 为 Info/Warning/Error/Fatal | ErrorCode 构造当前直接持有字符串，不等于 StrongId 校验；Error 仍是可写 DTO，const cause 指针不证明所有外部别名都不可变或无环。各入口/持久化的 cause 深度、环、字段与未知枚举处理必须独立核验 |
| [result.hpp](../../include/lasercnc/foundation/result.hpp)：Result&lt;T&gt;、Result&lt;void&gt; | success/failure、hasValue、显式 bool；T 的 value 有 &/const&/&& 重载，error 有 const&/&&；void 无 value 方法。模板拒绝引用和 Error 元素类型 | 错误分支读取 value 或成功分支读取 error 会抛 logic_error；分配、拷贝/移动异常不被 Result 容器自动吸收，不承诺整个接口 noexcept |
| [strong_id.hpp](../../include/lasercnc/foundation/strong_id.hpp)：StrongId&lt;Tag&gt;、StrongIdHash | create(string) → Result；value → string_view，默认三路比较；Hash 使用 string_view hash；不同 Tag 不互相转换 | 当前按字节使用 iscntrl/isspace 拒绝空/空白/控制字符，无统一长度或 UTF-8 校验；不得当作路径、认证凭据或跨进程持久稳定 hash。locale 对准入是否有影响仍须回归，不能从 ASCII 用例推导任意编码 |
| [version.hpp](../../include/lasercnc/foundation/version.hpp)：Version | major/minor/patch 三个 uint32_t，均默认 0；toString、默认三路比较 | 没有字符串 parse 或自行协商版本接口；0 值是否合法由所属契约决定，不能从 DTO 构造推出任何协议版本已受支持 |
| [schema.hpp](../../include/lasercnc/foundation/schema.hpp)：SchemaId、SchemaKind、Schema、ISchemaValidator | SchemaId 是 StrongId 标签；SchemaKind:uint8_t 为 Any/Null/Boolean/Integer/Number/String/Array/Object；create(id,version,rootKind,constraints=空Object,unit=无)；只读 id/version/rootKind/constraints/unit；validate(Schema,Value) | C6b3 已实测原实现将未知类型弱化为 Any，并修复为先检查八种具名枚举，其余返回 Foundation.SchemaKindInvalid，再检查 constraints 为 Object、unit 非空/非全空白；只有显式 Any 允许后端省略 type。见 [准入契约](ST1C6b3-Schema根类型准入.md)，不把根类型修复等同于完整 JSON Schema/预算签核 |
| [serialization.hpp](../../include/lasercnc/foundation/serialization.hpp)：IValueSerializer | 虚析构；const serialize(Value) → Result&lt;string&gt;，const deserialize(string_view) → Result&lt;Value&gt; | 未携带统一预算或取消参数；输入字节/节点/深度及输出预算不能只依赖某个 JSON 库默认值，C6c 必须跨端点落实 |

## Observability 声明与后续验证索引

下列五个头的声明和 src/runtime/observability 实现已静态核对。旧行为用例在 [observability_tests.cpp](../../tests/unit/runtime/observability_tests.cpp)；本轮未为以下待查问题新增回归，不把本轮全量旧用例通过解释为这些问题已经关闭。

| 文件与声明 | 字段及公开操作 | 已知边界与必须补查 |
| --- | --- | --- |
| [log_service.hpp](../../include/lasercnc/observability/log_service.hpp)：LogLevel、LogContext、LogRecord、ILogService | Level:uint8_t 六种 Trace/Debug/Info/Warning/Error/Critical；Context 七个 optional&lt;string&gt;：sessionId/projectId/commandId/taskId/workflowId/correlationId/traceId；Record 为 timestamp、level=Info、module/category/message、context、structuredData:Object；write/flush → Result&lt;void&gt; | Context 是普通文本观察字段，不是 StrongId 或授权上下文。C6b7 已用真实 human/JSONL 负例修复未知 level 提前拒绝为 InvalidLevel，并在格式转换前拒绝纪元前时间为 InvalidTimestamp。单记录文本/Value 和时间点边界归 C6c，不把文件轮转阈值当作记录预算 |
| [metrics_service.hpp](../../include/lasercnc/observability/metrics_service.hpp)：MetricKind、MetricLabels、MetricObservation、MetricSnapshot、IMetricsExporter/Service、LocalMetricsService | Kind:uint8_t Counter/Gauge/Histogram；Labels=有序 string→string；Observation 为 name/kind=Counter/value=0/labels/timestamp；Snapshot 为 name/kind/labels/count=0/value/sum/minimum/maximum=0。addCounter/setGauge/observeHistogram；构造 seriesCapacity=1024、failureCapacity=256，addExporter、freeze/frozen、snapshot、exporterFailures | C6b6 修复聚合溢出；C6b11 保证 exporter Error/异常的诊断 OOM 不跳过后续 exporter，聚合与成功 Result 保持；C6b12 用冻结数量和逐项所有者取得消除完整 exporter 向量复制。labels 字节总量及系列内容预算仍须审计；series 数量有界不等于每条内容有界 |
| [trace_service.hpp](../../include/lasercnc/observability/trace_service.hpp)：TraceStatus、TraceSpanStart/Record、ITraceExporter/Span/Service、LocalTraceService | Status:uint8_t Running/Succeeded/Failed/Cancelled/Stale；Start 为 traceId/spanId/parentSpanId/name/attributes；Record 再含 startedAt/finishedAt/status=Running/error。startSpan → Result&lt;unique_ptr&lt;ITraceSpan&gt;&gt;；Span id 只读、end(status,error) noexcept。构造 recordCapacity=4096、failureCapacity=256，addExporter、freeze/frozen、records/exporterFailures/activeSpanCount | C6b8/b9 修复准入与完成资源孤儿；C6b11 保证 exporter 失败诊断 OOM 不跳过后续 exporter；C6b12 令已保留完成记录不再被完整 exporter 快照分配打断。未完成 span 总量仍待核验；完成记录容量不限制 active 集合 |
| [diagnostics_service.hpp](../../include/lasercnc/observability/diagnostics_service.hpp)：DiagnosticStatus/Report、IDiagnosticCheck/Exporter、DiagnosticsService | Status:uint8_t Healthy/Degraded/Unhealthy/Unknown；Report 为 id/status=Unknown/summary/details/observedAt；Check.run → Result&lt;Report&gt;，Exporter.exportReport → Result&lt;void&gt;。构造 failureCapacity=256；registerCheck/addExporter、freeze/frozen、run/runAll/latest/exporterFailures | C6b10 规定同项串行/latest；C6b11 保证 exporter 失败诊断 OOM 不跳过后续通知；C6b12 使 latest 与冻结出口数量共同发布，取消完整向量复制。检查数量、details 总量和等待取消/deadline 仍待核验 |
| [log_observability_exporter.hpp](../../include/lasercnc/observability/log_observability_exporter.hpp)：LogObservabilityExporter | create(shared_ptr&lt;ILogService&gt;) → Result&lt;shared_ptr&lt;Exporter&gt;&gt;；exportSpan、exportObservation → Result&lt;void&gt;；实现 ITraceExporter 与 IMetricsExporter | null log service 为 Observability.InvalidLogService；持有共享日志寿命，转为结构化 LogRecord 并返回 write 结果，不额外 flush。C6b7 已对直接 DTO 的未知枚举/非有限数、名称/时间顺序、差值溢出及 write 抛异常取证修复，见 [出口契约](ST1C6b7-日志出口与等级准入.md)。正常返回 Error 保留；文本/Value/日期支持范围与全局资源预算仍须独立核验 |

## Messaging 声明与后续验证索引

声明依据两个公共头；分发实现为 src/runtime/messaging/event_bus.cpp，旧测试入口 [event_bus_tests.cpp](../../tests/unit/kernel/event_bus_tests.cpp)。以下同时区分私有构造权和持有值后的公开操作，不从“事实类型不可随意构造”推导消息本身不可再次发布。

| 文件与声明 | 字段及公开操作 | 已知边界与必须补查 |
| --- | --- | --- |
| [domain_event.hpp](../../include/lasercnc/messaging/domain_event.hpp)：PendingDomainEvent、CommittedDomainEvent | Pending：name/version/aggregateId:optional/payload；Committed：只读 name/version/aggregateId/payload/transactionId/projectId/documentId/revisions/sequence。构造仅友元 TransactionManager/PersistenceService | 事务提交和恢复认证负责事实生成；EventBus 消息观察不是新的事务或业务写入。事件版本/序列/aggregate 与 payload 的预算、持久 wire 对应及重发语义仍须跨事务/持久族核验 |
| [event_bus.hpp](../../include/lasercnc/messaging/event_bus.hpp)：EventKind、DeliveryMode、TransientEvent、EventEnvelope | Kind:uint8_t Domain/Notification/System；Mode:uint8_t Immediate/Queued；Transient 仅 notification(name,version,payload,optional coalescingKey) 与 system(name,version,payload)。Envelope 只读 kind/name/version/payload，以及 optional aggregateId/transactionId/projectId/documentId/revisions/sequence/correlationId/traceId/coalescingKey，构造仅 EventBus | Domain envelope 携带提交身份；Transient 没有事务/修订身份。C6b4 已复现并修复 Notification 跨版本合并，现按订阅实例/name/完整 version/coalescingKey 隔离；同版本仍更新原位置。见 [消息契约](ST1C6b4-消息准入与订阅身份.md)，不将观察通知合并推导为事务重放或任意版本兼容 |
| 同上：EventFilter、EventDeliveryFailure/Report、EventCallback | Filter：optional kind/name；Failure：subscriptionId/error；Report：matched/delivered/queued/coalesced 四个 size_t 默认 0、failures 数组；Callback=function&lt;void(const EventEnvelope&)&gt; | Report 的 Result 成功不等于全部 subscriber 成功；失败在 failures 内，Queued 更不等于已送达。计数、错误总量和回调 payload 寿命需声明；不能在回调返回后保留指向临时 envelope 的引用 |
| 同上：EventSubscription、EventBus | Subscription 不可复制、可移动，id/cancel noexcept，析构取消；Bus 不可复制，subscribe(id,filter,mode,callback) → Result&lt;Subscription&gt;；两种 publish → Result&lt;Report&gt;；drainQueued(maximumDeliveries=size_t(-1)) → Report；subscriptionCount/queuedCount | C6b4 拒绝未知 mode/filter.kind，并以私有实例身份防同名复用错投。C6b5 锁外释放捕获资源、锁外逐次复制并将复制异常按订阅隔离，见 [回调资源契约](ST1C6b5-消息回调资源与异常边界.md)。取消不排空已有快照，原件可延迟销毁；按值参数和全局 OOM 不承诺 noexcept。队列/订阅/合并 key 总量归 C6c/d |

## Workflow 与 Script 定义声明

| 文件与声明 | 字段及公开操作 | 已知边界与必须补查 |
| --- | --- | --- |
| [workflow.hpp](../../include/lasercnc/runtime/workflow.hpp)：WorkflowStepKind、WorkflowPredicateKind、Retry/Call/Step/Definition | StepKind 六种 Command/Query/WaitTask/Assign/Assert/Barrier；PredicateKind 五种 Exists/IsTrue/Equals/NotEquals/ArrayNotEmpty。Step 还含依赖、condition、command/query、模板与绑定、timeout、retry、compensation；Definition 含 descriptor、steps、resultTemplate | C6b13 在 registerDefinition 前白名单验证 StepKind 与所有可选 PredicateKind，未知值分别为 Workflow.InvalidStepKind/InvalidPredicateKind。图、引用和摘要既有规则不变；字符串、Value、节点/依赖/重试总量与持久状态/wire 仍须审计 |
| [script.hpp](../../include/lasercnc/runtime/script.hpp)：ScriptNodeKind、ScriptWaitTarget、Call/Wait/Include/Node/Definition | NodeKind 九种 Command/Query/Workflow/Wait/Assign/Assert/If/ForEach/Include；WaitTarget 为 Task/Workflow。Node 含五类可选调用、谓词、三组递归子节点、模板/绑定与 maxIterations；Definition 含 descriptor、nodes、resultTemplate | C6b13 在 registerDefinition 前白名单验证 NodeKind、复用的 PredicateKind及 WaitTarget，未知值分别为 Script.InvalidNodeKind/InvalidPredicateKind/InvalidWaitTarget。已有深度 32、节点 10000、单循环 10000 是局部定义限制，不等于 Value/字符串/执行总量和持久恢复预算已签核 |

C6b13 只修改 Registry 私有实现与错误分支，不改两份公共头、枚举底层值或定义 DTO；非法定义从“可注册、以后行为不确定/失败”收紧为组合期 Validation 拒绝。Workflow/Script 的 Request、Snapshot、状态机与 SQLite 检查点字段仍须在后续状态/持久族账本逐项登记。

## Task 与外部 Effect 的资源声明

| 文件与声明 | 字段及公开操作 | 已验证边界与后续缺口 |
| --- | --- | --- |
| [task.hpp](../../include/lasercnc/runtime/task.hpp)：ResourceKind、ResourceAccess、ResourceClaim | Kind 八种 CPU/DiskIO/GPU/OCCT/ProjectRead/ProjectWrite/MachineController/CollisionBackend；Access 为 Shared/Exclusive；Claim 含 kind、ResourceId、access、units=1 | C6b14 在配置、Effect 注册与 Task 仲裁处白名单验证枚举；未知值不建立槽、不注册命令、不取得资源或调用 handler。公共数字与字段不变；ResourceId 长度、声明/槽位总量归 C6c/C7 |
| [resource_manager.hpp](../../include/lasercnc/runtime/resource_manager.hpp)：ResourceAvailability、ResourceManager | Availability 为 kind/resource/capacity/sharedUnits/exclusivelyHeld；configure、freeze/frozen、snapshot、tryAcquire、release | configure 未知 kind 返回 Task.InvalidResourceKind 且快照不变；tryAcquire 在锁和槽位修改前拒绝未知 kind/access、零 units、冲突及重复 Shared 聚合溢出。ProjectWrite 继续规范化至 ProjectRead 并强制 Exclusive；capacity 支持上限未由本节点认证 |
| [command.hpp](../../include/lasercnc/runtime/command.hpp)：CommandDescriptor.resources | 外部 Effect descriptor 携带 ResourceClaim 数组 | 注册前拒绝未知 kind/access，分别为 Command.InvalidEffectResourceKind/InvalidEffectResourceAccess；零 units 既有拒绝保持。执行前仍经 ResourceManager，且先于 durable claim/handler；资源声明不取代 SideEffect、Guard、Idempotency 与 ReplayPolicy 权限准入 |

Task 的 ResourceManager 仲裁发生在提交被接受后的 Scheduler 泵内，所以非法声明形成可查询的 Failed 终态并保留原 Validation Error，handler 零调用；这不改成同步 submit 参数失败。C6b14 只关闭枚举闭集与单任务聚合算术，不代签等待公平、取消/deadline、全局容量或有界终态保留，见 [契约](ST1C6b14-资源声明枚举与算术准入.md)。

## Command/Query 版本解析策略

| 文件与声明 | 字段及公开操作 | 已验证边界与后续缺口 |
| --- | --- | --- |
| [execution_contract.hpp](../../include/lasercnc/runtime/execution_contract.hpp)：VersionResolution | uint8_t 枚举 Exact/Compatible；由 Registry descriptor 发现、CommandRequest、QueryRequest 使用 | C6b15 在 Command/Query Registry 查找前闭集验证，未知值分别为 Command.InvalidVersionResolution/Query.InvalidVersionResolution，不混为合法策略的 UnsupportedVersion |
| [command.hpp](../../include/lasercnc/runtime/command.hpp)：CommandRequest.versionResolution | 默认 Exact；参与进程内请求签名、持久命令签名及外部 Effect 签名 | 未知值不执行 handler/Schema/Capability/事务/幂等/Effect；失败 Trace 写 unknown。Exact/Compatible 正向签名与选择不变；请求/签名内容预算仍归 C6c/d |
| [query.hpp](../../include/lasercnc/runtime/query.hpp)：QueryRequest.versionResolution | 默认 Exact；Query 无幂等写签名 | 未知值不执行 handler/Schema/Capability/文档读取；失败 Trace 写 unknown。合法策略下 NotFound/UnsupportedVersion、resolvedVersion 和 Deprecated 保持 |

C6b15 不改公共头、枚举数字、DTO 或持久格式；它只收紧非法枚举的错误分类及观察字符串，见 [契约](ST1C6b15-版本解析策略枚举准入.md)。Version 数值、请求文本/Value、幂等及历史总量继续在后续预算和持久族审计中保留。

## Snapshot Store 写入证明

| 文件与声明 | 字段及公开操作 | 已验证边界与后续缺口 |
| --- | --- | --- |
| [snapshot_store.hpp](../../include/lasercnc/platform/snapshot_store.hpp)：SnapshotWriteDisposition、ISnapshotStore | disposition 为 Created/AlreadyPresent；writeAtomically、read、remove | C6b16 在 Persistence 索引前拒绝未知 disposition；AlreadyPresent 必须同 ID 读回并与待写 payload 精确相等，失败回滚且不留索引。Created 不强制读回，跨介质原子性与物理落盘不由该枚举证明 |
| [persistence_service.hpp](../../include/lasercnc/persistence/persistence_service.hpp)：captureSnapshot/latestSnapshot/recover | Snapshot 索引绑定项目、文档、修订、水位、storage key、digest、大小和时间 | 已存在内容读失败保留 cause，内容不匹配独立拒绝；孤立不可变文件允许保留但无索引即不可作为恢复锚点。快照数量、读取成本和环境支持继续归 C6c/C7/C5/ST1D |

C6b16 不改公共头、LCNCSN02、逻辑 payload 或数据库版本，见 [契约](ST1C6b16-快照存储写入证明准入.md)。错误适配器的历史错误索引不会自动修复，读取仍 fail-closed。

## Journal、Task 与 Workflow 持久 DTO 写前准入

| 公开入口 / DTO | 写入字段与版本 | C6b17 已验证边界与剩余项 |
| --- | --- | --- |
| [PersistenceService::append](../../include/lasercnc/persistence/persistence_service.hpp) / TransactionCommit | `lasercnc.state-journal` v4；ObjectChange 的 kind/objectId/before/after；HistoryMutation 的 kind/command/version/target/cursor | 写前拒绝未知 ObjectChangeKind/HistoryMutationKind；Created/Updated/Removed 与 Record/None/Barrier/Undo/Redo 的字段形状、对象身份和 Updated 类型一致性已验证，拒绝不建 Journal。事件内容、Version/Value/集合预算和全格式兼容清单仍待补齐 |
| acceptTask / TaskRequest；recordTaskTerminal / TaskSnapshot | `lasercnc.task-acceptance` v1；`lasercnc.task-terminal` v1；资源 kind/access 及终态 state/Error category/severity | acceptance 写前逐 ResourceClaim 闭集验证，未知 access 不再默认为 Shared；terminal 区分未知 TaskState 与合法非终态，顶层持久 Error 枚举闭集验证，拒绝后仍保留 Pending acceptance。Task terminal 既有 wire 不保存 cause，本轮不伪称已修复；请求/结果/错误内容预算和 cause 格式继续审计 |
| workflowDefinitionDigest；saveWorkflowCheckpoint / WorkflowDefinition、WorkflowSnapshot | definition/checkpoint/step 均为 v1；定义 StepKind/PredicateKind，实例/步骤状态，顶层/步骤/补偿 Error | 两入口共同拒绝未知定义枚举；checkpoint 写前拒绝未知 WorkflowState/WorkflowStepState，并验证全部已持久 Error 及既有 32 层 cause 范围内的 category/severity。拒绝不建实例/步骤记录；定义完整形状仍由 Registry 管理，字符串/Value/步骤总量及状态组合继续审计 |

C6b17 三项 Release 红灯覆盖 15 个分支，修复后 Release 全集 480/480、Debug 选集 225/225、ASan 675 次执行通过；公共头、枚举数字、SQLite schema 和上述 wire 版本均未修改，见 [契约](ST1C6b17-持久DTO写前准入.md)。直接构造 PersistenceService 仍只是独立组件能力，不获得 AppKernel Host 的业务写权限；历史非法材料不自动修复。

## 后续顺序（保留完整目标）

C6b5 登记的观察负例中，前三类由 C6b6 取得真实红灯并修复；以下按证据区分已补范围与剩余项：

- [LocalTraceService](../../src/runtime/observability/local_trace_service.cpp) 的 Running/未定义 end 已在 C6b6 归一；C6b8 修复准入分配失败孤儿；C6b9 修复显式完成及 abandoned 资源失败孤儿，明确活动释放强于尽力记录。活动总量仍待预算核验。
- [LocalMetricsService](../../src/runtime/observability/local_metrics_service.cpp) 的聚合溢出已在 C6b6 修复；C6b11 隔离 exporter 失败诊断 OOM，C6b12 取消完整 exporter 向量复制。输入字节/序列总量、时间戳与统一预算仍待审计。
- [DiagnosticsService](../../src/runtime/observability/diagnostics_service.cpp) 的状态/注册由 C6b6 修复，并发/latest 由 C6b10 修复；C6b11 隔离 exporter 失败诊断 OOM，C6b12 将 latest 与冻结数量共同发布。全局预算仍待核验。
- [日志观察出口](../../src/runtime/observability/log_observability_exporter.cpp) 和日志等级已在 C6b7 取得六项红灯并修复，包含未知枚举、数值/名称/时间、极端差值、后端异常和真实文件无副作用；合法 DTO/等级、原 Error 与 epoch 保留。不能把局部准入推导为全部日期或资源失败已认证。

以上与观察身份淘汰复用、活动总量、时间顺序和资源预算账本并存；不能以修好某一个枚举后删掉其他必须项。

1. C6b2–b12 检查点保留；C6b13–b17 补编排定义、资源声明、版本解析策略、快照写入证明及持久 DTO 写前准入，见 [最新交付](../阶段交付/2026-09-05-ST1C6b17-持久DTO写前准入.md)。下一步补 Host、State、Task Error cause 与其他持久族 DTO/格式；不以局部修复代签其他入口。
2. 继续 Foundation、Observability、Messaging 已登记的行为缺口及 Host/执行/状态/持久族逐类型、枚举和 DTO 字段；将源码兼容、权限阶段、线程/寿命、Error cause 和 wire 版本分别关联到测试。已修复的 Schema/消息枚举与合并/错投仅覆盖各子节点列出的范围；观察记录的未知枚举、数值/终态/寿命问题不因“非业务真值”而免审。
3. C6c/d 执行统一预算、同步与 Task、终态保留；C7 测容量，C8 完整日志/脚本/私有头门禁，ST1D 最终三配置签核。上述均未被本轮 8 个 Adapter 声明登记替代。
