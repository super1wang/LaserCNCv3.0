# Phase 3 基础设施端口与依赖治理

Phase 3 把成熟通用能力放在可替换 Adapter 后面。Kernel 只拥有产品语义和端口，第三方库只提供实现。

## 已冻结的端口

- `ILogService`：接收包含时间、级别、模块、分类、上下文和结构化数据的 `LogRecord`。
- `IValueSerializer` / `ISchemaValidator`：只接收 Kernel `Value` 和 `Schema`。
- `IConfigSerializer`：负责人工可编辑配置文本与 Kernel `Value` 之间的转换。
- `IPersistenceBackend`：提供参数化语句、行结果和底层事务能力；仅允许 Kernel PersistenceService/Repository 使用，领域模块不得散布语句。
- `ITaskExecutor`：只提交工作与完成回调；TaskId、优先级、取消、资源和持久化仍由后续 TaskRuntime/Scheduler 掌握。

## 依赖固定规则

GitHub 依赖同时记录发布版本与不可变提交。SQLite 使用官方 amalgamation，并以官方 SHA3-256 校验下载内容。任何升级都必须单独评审、更新依赖清单并重新运行 Debug、Release、Production-only 和 Adapter 测试。

生产依赖获取显式禁用仓库子模块。文档主题、上游测试框架和示例依赖不属于 LaserCNC 生产构建，也不得因 FetchContent 默认行为被递归拉取。

## 编译隔离

- jsoncons、toml11 和 BS::thread_pool 的头文件只允许出现在对应 Adapter 的 `.cpp` 或私有实现中。
- SQLite C API 只允许出现在 `infrastructure/persistence/sqlite/`。
- spdlog 类型只允许出现在 `infrastructure/logging/spdlog/`。
- Adapter 对外头文件仍只暴露 LaserCNC 接口；实现优先使用 PImpl，防止重模板头文件扩散。

## 当前子节点状态

依赖锁定和 Kernel 端口已建立。JsonconsAdapter 与 TomlConfigAdapter 已实现；spdlog、SQLite 和线程池 Adapter 仍在进行，Phase 3 尚未验收。

2026-09-03 验证：

- 五项 Production 依赖均按固定提交或官方 SHA3-256 完成 FetchContent 获取；
- SQLite 与 spdlog Debug/Release 实体库构建通过；
- jsoncons、toml11、BS::thread_pool 的固定版本头文件入口已验证；
- Kernel 端口替换性测试通过；
- VS2022 x64 Debug 与 Release 均通过 23/23 CTest；
- 首次获取暴露了 toml11 上游文档/测试子模块的无关递归下载，现已在所有 Git 依赖声明中明确禁用子模块。

JSON/TOML 子节点验证：

- Kernel Value 的 Null、Boolean、Integer、Number、String、Array、Object 完整 JSON 往返通过；
- 超出 `int64_t` 的 JSON 无符号整数和语法错误均转换为 Kernel Error；
- JSON Schema 的有效/无效实例验证通过，jsoncons 类型未进入公共头文件；
- TOML 对象配置解析、序列化和二次解析一致性通过；
- TOML 无法表达的 Null 以及 Kernel 暂不支持的日期时间值均 fail-closed；
- VS2022 x64 Debug 当前通过 28/28 CTest。
