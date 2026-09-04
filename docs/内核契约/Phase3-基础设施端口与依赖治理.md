# Phase 3 基础设施端口与依赖治理

状态：已验收（2026-09-03）。

Phase 3 把成熟通用能力放在可替换 Adapter 后面。Kernel 只拥有产品语义和端口，第三方库只提供实现。本阶段没有创建 CAD、CAM、GUI、CLI、Workflow 或其他上层模块。

## 已冻结的端口与实现

| Kernel 端口 | Infrastructure 实现 | 第三方边界 |
|---|---|---|
| `ILogService` | `SpdlogLogService` | spdlog |
| `IValueSerializer`、`ISchemaValidator` | `JsonconsAdapter` | jsoncons |
| `IConfigSerializer` | `TomlConfigAdapter` | toml11 |
| `IPersistenceBackend` | `SqlitePersistenceBackend` | SQLite C API |
| `ITaskExecutor` | `BsThreadPoolExecutor` | BS::thread_pool |

`IPersistenceBackend` 仅允许后续 Kernel `PersistenceService` 或 Repository 使用，领域模块不得散布 SQL。`ITaskExecutor` 只负责运行工作和发出完成通知；TaskId、优先级、取消、资源仲裁、进度和持久化仍由后续自研 TaskRuntime/Scheduler 掌握。

## 日志契约

`SpdlogLogService::create` 在成功后至少拥有一个输出。可启用控制台、轮转人工日志文件和轮转 JSONL 文件；两个文件不得指向同一路径，文件大小和保留数量不得为零。

ST1C6a 补充：两个文件路径均先检查非空及原生码元内的 NUL，再创建任何 sink；NUL 不得截断成另一个实际文件名。Windows 使用统一重建的宽字符 spdlog 配置，实际验证中文/emoji 路径及轮转文件。该预检不承诺任意 I/O 失败时的跨文件回滚；Windows 路径别名与不可转换输入继续按 [C6 契约](ST1C6-公共契约与输入预算.md) 实测审计，不能将旧配置用例当作所有别名已认证。

ST1C6b1 补充：五类文件路径统一拒绝未配对 UTF-16 代理码元，错误沿用既有 InvalidOptions/cause。无法转换的 Snapshot 路径诊断明确省略 path 并提供 pathEncoding="invalid-utf16"、pathOmitted=true，不能为生成错误再次转换坏路径。合法码点边界和两类日志混合预检已有回归；底层 Windows 能保存某些异常原生名称不等于内核承诺接纳这些名称。字段/操作与剩余别名、预算边界见 [逐项账本](ST1C6b-公共契约逐项审计.md)。

每条 JSONL 日志是一行完整 UTF-8 JSON，包含：

- `timestamp`：使用调用方 `LogRecord` 的时间戳，规范化为 UTC 毫秒格式；
- `level`、`module`、`category`、`message`；
- 已提供的 `sessionId`、`projectId`、`commandId`、`taskId`、`workflowId`、`correlationId`、`traceId`；
- `structuredData`：原样保留 Kernel `Value::Object`。

人工日志保留相同的关键上下文，方便终端和文件直接阅读。写入与刷新是同步调用；spdlog sink 错误转换为 Kernel Error。多个 sink 不是跨文件事务：若后一个 sink 失败，前一个 sink 已写入的记录不会回滚，调用方会收到 `Logging.WriteFailed`。

## JSON 与 TOML 契约

- JSON 完整支持 Kernel `Value` 的 Null、Boolean、Integer、Number、String、Array、Object 往返。
- 超出 `int64_t` 范围的 JSON 整数、语法错误和 JSON Schema backend 错误统一转换，不泄漏异常。
- Kernel `Schema` 由 Kernel 拥有；jsoncons 只负责 JSON Schema 编译和校验。
- TOML 根必须是 Object。TOML 无法表达的 Null 与 Kernel 暂不支持的日期时间类型均 fail-closed，不做隐式有损转换。

## SQLite 契约

`SqlitePersistenceBackend` 是轻量 RAII backend，不是 ORM，也不是 Application Transaction。每个实例拥有一个连接，使用 SQLite `FULLMUTEX` 并在 Adapter 内串行化连接调用；默认 busy timeout 为 5000 ms，外键约束默认开启。

语句规则：

- 每次调用只允许一条非空 SQL，禁止嵌入空字符和尾随第二条语句；
- 占位符数量必须与参数数量完全一致；
- 参数只接受 Null、Boolean、Integer、Number、String；Array/Object 必须由上层 Repository 依据显式 schema 先序列化；
- `query` 在真正执行前拒绝不返回列的语句，避免用查询入口隐式执行无结果写入；查询结果映射为 Kernel `Value`，重复列名必须先由调用方设置唯一别名；
- BLOB 不进入通用 `Value` 通道。大型几何和二进制资产属于文件 Data Plane，SQLite 只保存 Control Plane 元数据；
- `execute` 在真正执行前拒绝会返回行的语句，避免 `RETURNING` 产生“已经修改但返回错误”的歧义；返回值是该语句导致的连接总行变化增量，包含 trigger/外键动作；
- `beginTransaction`、`commitTransaction`、`rollbackTransaction` 依据 SQLite autocommit 真值校验状态。重复 begin 或无活动事务的 commit/rollback 返回冲突错误；析构时回滚未提交的底层事务。

SQLite Transaction 只提供持久化原语。Revision、Undo、Domain Event、提交后发布与业务一致性边界仍属于后续 Application Transaction。

## 线程池契约

`BsThreadPoolExecutor` 仅作为 `ITaskExecutor` backend：

- `submit` 成功表示工作已被接收；每个已接收工作运行一次，并调用完成回调一次；
- 工作返回的 Kernel Error 原样传给完成回调；工作抛出的异常转换为 `Execution.WorkThrew`；
- 完成回调是通知边界，禁止抛异常。Adapter 会隔离意外异常，防止其逃逸到第三方 worker loop 并破坏后续任务；
- `waitIdle` 等待当时队列与正在运行的工作完成；
- `shutdown` 先停止接收，再排空已接收工作；它是幂等的，并串行化并发关闭者；关闭返回后不会再有完成回调；
- 从本 Executor 的 worker 内同步调用 `waitIdle` 或 `shutdown` 会形成自等待，因此 fail-closed；
- `threadCount == 0` 使用 BS::thread_pool 的平台默认并发度。

Executor 对象必须由组合根持有，并存活到 `shutdown` 返回。优先级、取消与任务状态不能绕过后续 Scheduler 直接委托给 BS::thread_pool。

## 统一错误

| 范围 | 代表错误码 |
|---|---|
| Logging | `Logging.InvalidOptions`、`Logging.InitializeFailed`、`Logging.SerializationFailed`、`Logging.WriteFailed`、`Logging.FlushFailed` |
| Serialization / Config | `Serialization.JsonEncodeFailed`、`Serialization.JsonParseFailed`、`Serialization.SchemaBackendFailed`、`Config.ParseFailed`、`Config.SerializeFailed` |
| Persistence | `Persistence.InvalidOptions`、`Persistence.DatabaseOpenFailed`、`Persistence.DatabaseFailed`、`Persistence.ParameterCountMismatch`、`Persistence.UnsupportedParameter`、`Persistence.UnsupportedColumnType`、`Persistence.NoResultColumns`、`Persistence.TransactionStateConflict` |
| Execution | `Execution.InitializeFailed`、`Execution.InvalidWork`、`Execution.InvalidCompletion`、`Execution.WorkThrew`、`Execution.ExecutorStopped`、`Execution.WaitFromWorkerDenied`、`Execution.ShutdownFromWorkerDenied` |

所有第三方异常和错误码都在 Adapter 边界转为 `foundation::Error`。第三方原始原因只作为 details 或 cause 留存，不进入控制流契约。

## 依赖与编译隔离

GitHub 依赖同时记录发布版本与不可变提交。SQLite 使用官方 amalgamation，并以官方 SHA3-256 校验下载内容。任何升级都必须单独评审、更新依赖清单并重新运行 Debug、Release、Production-only 和 Adapter 测试。

生产依赖获取显式禁用仓库子模块。文档主题、上游测试框架和示例依赖不属于 LaserCNC 生产构建，也不得因 FetchContent 默认行为被递归拉取。

- jsoncons、toml11 和 BS::thread_pool 的头文件只允许出现在对应 Adapter 的 `.cpp` 私有实现中；
- SQLite C API 只允许出现在 `infrastructure/persistence/sqlite/`；
- spdlog 类型只允许出现在 `infrastructure/logging/spdlog/`；
- Adapter 公共头文件只暴露 LaserCNC 接口与标准库类型；
- CTest 架构门禁同时扫描公共 API 泄漏和生产源码中的三方实现目录越界。

## 验收证据

C6b2 补充：[日志身份与轮转准入](ST1C6b2-日志文件身份与轮转准入.md) 将 Windows 文件元数据查询放在 persistence/filesystem/windows，spdlog 的命名与冲突编排留在 logging/spdlog。公共 Options/方法及依赖锁定版本不变；新增路径准入与失败语义按该契约执行，以下 Phase3 历史成绩不自动覆盖新增代码。

C6b3 补充：[Schema 根类型准入](ST1C6b3-Schema根类型准入.md) 先在 Foundation 工厂拒绝未知枚举，jsoncons 仅对显式 Any 省略 type。合法 Any 的其他约束继续执行；Unknown 不得隐式降级。此改动不代替所有 Schema 关键字、数值、编码或统一预算的验证。

2026-09-03：

- 五项 Production 依赖均按固定提交或官方 SHA3-256 获取，版本、许可证和来源统一记录；
- JSON/TOML 完整往返、非法输入、schema 校验与 fail-closed 路径通过；
- 人工日志与 JSONL 字段、UTC 时间、flush、配置错误和 spdlog 初始化错误转换通过；
- SQLite 参数绑定、标量结果、真实文件重开、外键开关、事务 commit/rollback、非法 SQL、复合值/BLOB、重复列和多语句拒绝通过；
- 线程池成功/失败/异常工作、回调隔离、排空关闭、关闭后拒绝、worker 自等待拒绝与并发关闭通过；
- VS2022 x64 Debug 与 Release 均通过 40/40 CTest；
- Debug 全集连续 20 轮通过；
- `LCNC_BUILD_TESTING=OFF` 的独立 Release 构建通过，未生成 Catch2 源码或测试目标。

详细命令与边界说明见 [`阶段交付/2026-09-03-Phase3-Infrastructure-Adapters.md`](../阶段交付/2026-09-03-Phase3-Infrastructure-Adapters.md)。
