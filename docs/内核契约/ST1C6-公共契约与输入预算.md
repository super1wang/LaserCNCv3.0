# ST1C6 公共契约与输入预算

## 目标和执行顺序

承接 [ST1C 补充计划](ST1C-补充审计与剩余执行计划.md)，起点 `c7b3d87`。C6 必须同时收口 API/DTO/持久格式兼容、同步调用/Task 契约、统一输入预算及有界终态保留。不能只修一个路径问题、增加一个表格或限制观察日志就签核整个节点；最终容量证据由 C7 支持，最终三配置由 ST1D 签核。

| 子节点 | 实施与验收 | 状态 |
| --- | --- | --- |
| C6a | 公共文件路径精确性：NUL 在 OS/库调用前拒绝，保留合法 Unicode，多个日志输出先完成参数预检；对真实副作用取得红灯并修复 | 本地检查点通过：四项新测试原实现 0/4；修复后日志/路径 6 项各三次、全量 Debug 412/412、定向 ASan 54 项各三次、纯生产/架构通过，见 [交付](../阶段交付/2026-09-04-ST1C6a-文件路径精确性与Unicode日志.md) |
| C6b | 逐公共入口/DTO/错误 cause/持久版本清单；配置期、执行期、诊断、生命周期与内部恢复的可达权限区分；明确源码兼容和回滚边界 | [71 公共头基线](Kernel-1.0-公共头清单.md) 与 [声明账本](ST1C6b-公共契约逐项审计.md) 保留；最新 [C6b15 版本解析策略枚举准入](ST1C6b15-版本解析策略枚举准入.md) 拒绝 Command/Query 未知 VersionResolution 并精确记录失败观察，C6b14 资源规则保留。下一步补 Host、状态与持久族类型/字段/格式；不声称已冻结 |
| C6c | Value/Schema/序列化/配置与命令、查询、任务、工作流、脚本及恢复材料的统一尺寸、深度、数量和累计预算；输入/输出均检查，拒绝不留部分提交 | 待实现；现有局部限制不得代签统一预算 |
| C6d | 同步调用与 Task 的可取消/超时边界；活动、终态发布、身份/幂等保护及有界内存保留 | 待实现与验证；不得靠删除未完成记录或丢失防重身份满足“有界” |
| C6 汇总 | 各子项闭合、兼容清单可检查、负例/全集/ASan/生产/架构通过后作为远端大节点 | 未达到；子节点独立本地提交 |

本阶段仅内核，不添加工程包解析、CAD/CAM/Process/Machine、GUI、产品 CLI/RPC/AI。可改变内核公共输入契约以满足明确预算，但必须记录兼容变更、端点范围和拒绝语义，不能用“调用者不要传大数据”代替实际门禁。

## 公共契约审计索引（不等同于已签核）

| 契约族 | 当前文件/类型入口 | C6b 必须核验 |
| --- | --- | --- |
| 基础值与错误 | foundation 的 Value、Schema、StrongId、Version、Error/Result、IValueSerializer/ISchemaValidator | 合法值域、编码/空值、数值范围、深度/总量、Error cause、异常与 Result 的边界；Schema 形状不是资源预算 |
| Host 装配与治理 | AppKernel、Module/ModuleRuntime/ModuleRegistrar、ServiceRegistry | 配置顺序、所有权、Registry freeze、独立组件与 Host 暴露面的区别；Failed/Stopping/析构约束 |
| 执行 DTO | ExecutionGateway/ExecutionCatalog；Command/Query/Task/Workflow/Script 的 Request、Response、Descriptor、Snapshot | 版本、Scope/Capability、SideEffect/Recovery policy、LifecycleOperation、调用链和输出；私有构造/委托与普通 Host 可调用入口不能混为一谈 |
| 状态与生命周期 | ProjectRuntime、DocumentRuntime、DocumentStore、ObjectTypeRegistry、RevisionSet、History、CatalogVersion | 0:N 归属、业务修订与目录令牌的区别、认证恢复、Removed/Detached 历史域、Open/Failed 准入和源兼容 |
| 消息与观察 | DomainEvent/EventBus、Trace/Metrics/Diagnostics、LogService | 候选成功事件与失败诊断、同步回调/排队、生命周期、容量及观察失败对业务提交的影响 |
| 持久端口与 DTO | PersistenceService 的 Journal/Snapshot/Catalog/Recovery/Idempotency/ExternalEffect/WorkflowCheckpoint/SessionStatus；platform 六端口 | 独立组件写接口不能视为 Host 可变入口；原始存储格式、读写版本、迁移与回滚、所有权和历史根认证 |
| Infrastructure options | SQLite、Snapshot/Asset、Spdlog、JSON/TOML、线程池适配器 | 参数精确性、有效默认值、系统路径/编码、字节预算、库/OS 异常转换、配置实际读回、可信存储根 |

公共头清单包含 7 foundation、8 infrastructure、7 kernel、2 messaging、5 observability、1 persistence、6 platform、29 runtime、6 state，共 71 份。文件摘要只能发现差异，不能证明 DTO 字段、枚举语义或线程/生命周期兼容。C6b 须为每个公共类型/操作提供所属文件、权限阶段、输入输出/错误、持久关联和测试入口；无需在本阶段引入跨工具链 C++ ABI 或动态加载器。

## 已知兼容变化与待冻结项目

- C4 移除公开 attach：旧 Host 必须使用空身份 create、受治理 Command/Transaction 导入及可信持久 open；不提供兼容别名或恢复写旁路。
- C2 的 LifecycleOperation、CatalogVersion、Project/Document 目录 DTO 与持久执行历史根认证：令牌不是业务 Revision 或授权；关闭控制不与普通执行租约自阻塞。
- C3 的 LCNCSN02、固定文件键、4096 字节身份预算与最新快照/完整恢复共用认证；只有前向读取兼容，旧二进制须配套旧格式完整归档，不能原地回退。
- C4/C5 的 Failed Host、Interrupted Opening、只读诊断材料与成套离线恢复：[C5c 支持矩阵](ST1C5c-存储支持矩阵与离线恢复.md) 明确未验证环境及不自动修复规则。
- C6a 的 NUL 路径拒绝与日志 Unicode 修复不新增公共字段或持久版本，但原先依赖路径截断的调用将被拒绝；原乱码日志不会自动迁移。宽字符编译选项必须在同一构建内同时用于 spdlog 与适配器，不混用旧库二进制。

持久版本清单必须分别审查数据库迁移、Journal、Snapshot 逻辑负载及外层信封、ObjectRecord、Idempotency、Task/Effect、Workflow、Diagnostic、Project/Document 目录和执行历史根，不能只抄一个数据库版本数字。未知/损坏版本不得按默认值接纳；迁移不自动重放不安全副作用。

## C6a 路径精确性规则

五个选项为 SqliteConnectionOptions.databasePath、FilesystemSnapshotStoreOptions.directory、FilesystemAssetStoreOptions.directory、SpdlogLogOptions.rotatingFilePath/jsonlFilePath。

1. 在原生路径码元上检查内嵌 NUL，先于 UTF-8 转换、绝对化、创建目录、SQLite open 或日志 sink 构造；既有合法路径和 Unicode 不应被改写为另一文件名。
2. 日志两类路径都预检完成后才创建任何 sink，不能因第一个参数合法就先创建文件。该规则只针对已知参数错误，不承诺任意后续 I/O 失败时跨多个 sink 的文件系统原子回滚。
3. SQLite 返回 Persistence.InvalidOptions；Snapshot 返回 Snapshot.InvalidStoreOptions；Asset 保留 Asset.StoreInitializationFailed → Snapshot.InvalidStoreOptions 的既有包装层；日志返回 Logging.InvalidOptions。错误诊断不应调用被拒绝路径的文件 API。
4. Windows spdlog 使用官方 SPDLOG_WCHAR_FILENAMES 和原生宽字符路径，依赖及适配器同时重建；公共 Kernel API 不出现 spdlog 类型。正常 Unicode 数据库/快照/资产/日志读写以及日志轮转必须实际验证。
5. 新目标拒绝不能创建截断前缀，既有目标拒绝不能更改原内容或增添输出；合法 Unicode 对照不能只有返回成功，必须按期望的原生路径读取文件。失败现场和乱码文件保留为证据，不触碰用户真实文件。

这不是文件根信任、链接替换、非法 UTF-16、任意路径长度或权限的完整认证；这些条件在 C6b/c 与 C5 支持矩阵继续逐项核对，不能因单一 NUL 检查宣称所有路径输入已安全。

C6b1 已实测确认 SQLite 不可转换的 UTF-16 会逸出 Result，其他文件适配器会接受并创建异常原生名称；已统一拒绝未配对代理码元，并使 Snapshot 错误诊断不再转换坏路径，具体兼容变化与证据见 [逐项账本](ST1C6b-公共契约逐项审计.md)。C6b2 已实测并修复日志大小写/硬链接/父目录别名及轮转目标冲突，保留合法大小写敏感文件和不同尾随字符目录，见 [文件身份契约](ST1C6b2-日志文件身份与轮转准入.md)。这是稳定可信配置根的静态准入，不代签运行期替换、跨实例占用、任意 I/O 回滚或全部文件系统支持；统一门禁结果单独记录。

## 输入预算与保留审查的起始证据

JsonconsAdapter 和 TomlConfigAdapter 的 Value 转换当前使用递归，公共 serializer 端口未声明统一预算参数；不能把库默认 parser 限制当作内核全链预算。Snapshot/Asset 已有按字节的 option 上限，Workflow 错误 cause 有 32 层限制，Script Registry 有节点深度/节点数量/循环次数限制，这些都只是局部保证。

Workflow/Script 每实例最多观察 256 个步骤/节点，不等于 instances 集合或所有终态内存有界。C6d 应联合检查 Scheduler、Command 幂等缓存、Workflow/Script 实例及观察/持久终态：未结束、未发布、未持久确认的活动不得被清理；终态移出内存后的查询/重复身份/不安全重放行为必须仍有明确契约和测试。具体上限和优化需要 C7 的容量数据支撑，不提前虚构吞吐或内存 SLA。

C6b2 的日志预检枚举 base 与全部保留轮转目标，具有 O(N log N) 集合成本和逐目标元数据查询。锁定库允许的 200000 数值上限不是实测容量；C6c/C7 必须测量保留数、目录深度、已有文件规模与初始化耗时/内存，并据证据制定支持配置。Foundation 逐项账本登记的未知 SchemaKind 风险已在 C6b3 取得真实红灯并修复，规则与兼容性见 [根类型准入](ST1C6b3-Schema根类型准入.md)。消息风险由下述 C6b4 承接，之后推进剩余声明和统一预算，不用单个枚举修复代签所有值/错误/Schema 契约。

C6b4–b10 修复消息、观察资源及 Diagnostics 并发，[C6b11](ST1C6b11-观察出口失败记录资源隔离.md) 隔离三服务 exporter 失败记录 OOM，[C6b12](ST1C6b12-观察出口快照无分配发布.md) 消除完整 exporter 向量复制分配，[C6b13](ST1C6b13-编排定义枚举准入.md) 补 Workflow/Script 定义枚举，[C6b14](ST1C6b14-资源声明枚举与算术准入.md) 补资源声明准入，[C6b15](ST1C6b15-版本解析策略枚举准入.md) 补 Command/Query 解析策略与观察精确性。队列、资源声明与槽位总量、失效候选保留、共享实例/回调原件、观察与编排内容成本继续由 C6c/d/C7 收口。

同步 handler/validator/日志回调不会因设置 timeout 就获得可抢占中断能力。C6d 必须区分执行预算、等待超时、协作取消、Task 完成与最终 executor drain；不能将调用返回超时当成副作用已停止或资源可销毁。
