# ST1C6c1 共享 Value 与序列化预算

## 范围与结论

本节点承接 [C6 公共契约与输入预算计划](ST1C6-公共契约与输入预算.md)，只处理 Application Kernel 的共享 `Value` 结构预算、`Schema` 元数据准入以及 JSON/TOML 适配边界，不增加 CAD、CAM、Machine、Process、GUI、产品 CLI/RPC/AI 或工程包逻辑。

旧实现允许任意深度的 `Value` 进入 JSON/TOML 递归转换，也允许超深 `Schema::constraints` 成为已注册契约。现在形成一个可复用且只能收紧的 Kernel 硬上限，并在第三方解析/格式化前后实施失败关闭。C6c1 是 C6c 的第一个本地检查点，不代签命令、查询、任务、工作流、脚本、观察、错误 cause 或持久 DTO 的字段/集合累计预算；这些仍由 C6c2 继续。

## 共享硬上限与计数语义

| 维度 | Kernel 硬上限 | 计数规则 |
| --- | ---: | --- |
| `maximumDepth` | 64 | 根 `Value` 深度为 1；第 64 层允许，第 65 层拒绝 |
| `maximumNodes` | 100000 | null、布尔、整数、浮点、字符串、数组和对象本身均计一个节点 |
| `maximumTextBytes` | 16 MiB | 累计对象键和字符串值的实际字节数；数值和容器标记不计入文本 |
| `maximumEncodedBytes` | 64 MiB | JSON/TOML 编码输入或格式化输出的字节数；与解码后的文本预算分开 |

`assessValueBudget()` 返回已观察到的最大深度、节点数、文本字节数和首个违规维度。调用者可传入更小限制，但任一维度都不能高于 `kernelValueBudget`；试图放宽返回 `invalidBudget`，不会遍历材料。累计使用饱和加法，拒绝路径不会因 `size_t` 回绕变成成功。

这些数值是安全准入硬上限，不是吞吐、内存或业务推荐配置。C7 仍须在真实恢复、序列化和目录规模下测量成本，再决定支持配置及是否优化。

## 准入位置与失败语义

| 入口 | 检查时机 | 专用错误码 | 拒绝后的保证 |
| --- | --- | --- | --- |
| `Schema::create()` constraints | 保存 Schema 前 | `Foundation.SchemaBudgetExceeded` | 不生成半有效 Schema |
| `Schema::create()` unit | 空白扫描和保存前 | `Foundation.SchemaUnitBudgetExceeded` | 不扫描超限文本，不生成 Schema |
| `JsonconsAdapter::serialize()` | 构造 jsoncons DOM 前；编码后再检查输出 | `Serialization.ValueBudgetExceeded` / `Serialization.OutputBudgetExceeded` | 不返回部分 JSON |
| `JsonconsAdapter::deserialize()` | 复制/解析前检查输入；parser 深度及 DOM 转换继续检查 | `Serialization.InputBudgetExceeded` / `Serialization.ValueBudgetExceeded` | 不返回部分 `Value` |
| `JsonconsAdapter::validate()` | 构造 Schema backend 前检查 constraints 和输入值 | `Serialization.ValueBudgetExceeded` | 超限材料不进入第三方 Schema backend |
| `TomlConfigAdapter::parse()` | 建立 stream 前检查输入和 sourceName；DOM 转换继续检查 | `Config.InputBudgetExceeded` / `Config.SourceNameBudgetExceeded` / `Config.ValueBudgetExceeded` | 不返回部分配置值 |
| `TomlConfigAdapter::serialize()` | TOML 转换前检查 Value；格式化后再检查输出 | `Config.ValueBudgetExceeded` / `Config.OutputBudgetExceeded` | 不返回部分 TOML |

预算错误均为 `Validation` 类别，details 统一包含字符串形式的 `dimension`、`actual`、`limit` 和 `material`。既有未知 SchemaKind、非对象 constraints、空白 unit、JSON 语法、TOML 根类型/不支持类型等错误码保持原有分层；预算只在对应材料可能造成无界工作前取得优先级。

JSON parser 明确设置最大嵌套深度 64，DOM 到 `Value` 的共享转换预算同时限制深度、节点与文本。TOML 适配器在第三方 parser 前先以 64 MiB 限制原始输入，并在 DOM 转换时限制解码结构；toml11 自身没有接入统一节点/文本的流式提前终止，因此解析阶段仍可能产生受输入字节上限约束的放大，这一成本必须在 C7 测量，不能在本节点写成零放大或恒定内存。

## 公共契约与兼容边界

本节点仅修改一个公共头 [value.hpp](../../include/lasercnc/foundation/value.hpp)，新增 `ValueBudget`、`kernelValueBudget`、`ValueBudgetViolation`、`ValueBudgetAssessment`、`assessValueBudget()` 和 `valueBudgetViolationName()`。原有 `Value` 类型、字段顺序、存储变体及构造/读取接口未修改，因此没有修改 `Value` 对象布局；新增自由函数和类型仍是源码/API 增量，不宣称跨工具链 C++ ABI。

C6a 起始摘要 `BE0A6B38F7624654DE3011755B0C371A48E2FBA341BA80405CD00EF93ECC30B0` 变为 `80D58BA591CA66660D3418879F98BF61A6BB6A3AAEC5AE575A177EDFC7F77127`。其余 70 个公共头不变，完整登记见 [公共头清单的 C6c1 增量](Kernel-1.0-公共头清单.md#c6c1-公共头增量)。这不是最终冻结摘要；C6–C8 与 ST1D 完成后才能签发最终基线。

序列化和配置接口签名、JSON/TOML 正常编码形状、SQLite schema 及所有既有持久 wire version 均未变化。兼容变化是：此前可能成功的超限输入现在返回上述结构化错误；旧二进制不能据此被声明为具有同等资源防护。

## 红灯与验证证据

先在未修复实现加入深度负例：Release 下 JSON 与 TOML 两项均在 `REQUIRE_FALSE(serialized.hasValue())` 失败，旧实现实际返回成功。随后加入 Schema constraints 负例，旧实现同样在 `REQUIRE_FALSE(schema)` 失败。三项均为真实行为红灯；其余节点、文本、输入/输出字节和 sourceName 用例用于完整边界覆盖，不伪造旧缺陷结论。

修复后的门禁：

- Debug 全构建通过；预算焦点 8/8。
- Release 预算焦点 8/8；完整回归 498/498，通过时间 1081.59 秒。
- ASan 全构建通过；相同 8 项连续三轮，共 24 次执行，无 sanitizer 报告。
- production-only Release 构建通过。
- 边界脚本通过 71 个公共头、141 个生产源，模块治理、ExecutionGateway 旁路和第三方实现目录隔离均通过。

现有边界脚本仍不扫描 `src` 私有 `h/hpp`；本节点新增的 `value_budget_support.hpp` 已人工复核为 Infrastructure 私有实现，但该人工复核不冒充自动门禁。补齐私有头扫描与负向探针仍归 C8。

## 后续边界

C6c2 必须把同一结构评估接到真正持有 `Value` 的 Command/Query/Task/Workflow/Script、事件、观察和持久材料入口，并分别补齐非 `Value` 文本、身份、集合数量及跨字段累计预算。拒绝必须发生在注册、排队、事务、发布或持久写入前，不得只依赖 JSON/TOML 最终落盘时兜底。

C6d 继续处理同步回调、Task/编排取消与 deadline、活动/终态发布和有界保留；C7 测量容量与性能，C8 完善门禁，ST1D 进行最终重签。C6c1 不代签这些节点，也不构成 C6、ST1 或 Kernel 1.0 Frozen。
