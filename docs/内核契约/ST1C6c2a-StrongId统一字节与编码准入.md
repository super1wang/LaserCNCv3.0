# ST1C6c2a StrongId 统一字节与编码准入

## 范围与结论

本节点承接 [C6 公共契约与输入预算计划](ST1C6-公共契约与输入预算.md) 和 [C6c1 共享 Value 预算](ST1C6c1-共享Value与序列化预算.md)，只收口 Application Kernel 的 `StrongId<Tag>` 唯一构造入口。`kernel/identifiers.hpp` 中 35 类身份别名以及 Foundation/Schema、资产和快照等使用同一模板的身份，由此共享相同字节与编码准入；不修改上层模块或增加产品业务身份。

旧实现只拒绝空值及按当前 C locale 识别的空白/控制字节，会接受 4097 字节甚至更长的身份，也会接受过长编码、代理区编码和超出 Unicode 标量范围的畸形 UTF-8。现在统一为：最多 4096 字节、严格合法 UTF-8、非空且不含 ASCII 空白/控制字符。C6c2a 是 C6c 的局部检查点；注册定义、请求/事件/观察、错误和持久 DTO 的跨字段累计预算仍由 C6c2b–d 承接。

## 规范化准入顺序

`StrongId<Tag>::create()` 是所有强类型身份的唯一公开构造入口，按以下顺序失败关闭：

1. 字节数大于 `kernelStrongIdMaximumBytes == 4096` 时立即返回 `Foundation.StrongIdBudgetExceeded`，不继续扫描内容；恰好 4096 字节允许进入后续检查。
2. 严格检查 UTF-8：拒绝孤立 continuation、缺失 continuation、非法 leading byte、`C0/C1` 过长编码、`E0` 过长三字节、`ED A0..BF` 代理区、`F0` 过长四字节及 `F4 90..BF`/`F5..FF` 超出 U+10FFFF 的编码，返回 `Foundation.InvalidStrongIdEncoding`。
3. 保留既有非空、无空白/控制字符要求，返回 `Foundation.InvalidStrongId`。当前无需 ICU，语义精确为 ASCII/C locale 空白与控制字节；不宣称进行了 Unicode 规范化、大小写折叠或全体 Unicode 空白分类。

合法 UTF-8 字节保持原样，不做 NFC/NFD 合并，不改变大小写，也不把文本身份转成文件路径。`snapshot.é` 与以组合字符表示的视觉近似身份仍是不同精确字节；身份字符串本身不授予权限。

## 错误与副作用

| 条件 | 错误码 | details | 副作用边界 |
| --- | --- | --- | --- |
| 超过 4096 字节 | `Foundation.StrongIdBudgetExceeded` | `dimension=identityBytes`、`actual`、`limit`、`material=strongId` | 不生成身份，不进入注册表、请求、日志、hash 或持久入口 |
| 畸形 UTF-8 | `Foundation.InvalidStrongIdEncoding` | 无回显原始畸形字节 | 不生成身份，避免错误诊断再次编码不可信字节 |
| 空值/空白/控制 | `Foundation.InvalidStrongId` | 保持既有错误 | 不生成身份 |

超长且同时畸形的材料先返回预算错误；该优先级确保在固定字节上限前不做与内容长度成正比的编码扫描。拒绝错误不回显原始身份，避免大材料或畸形字节进入 `Error.details`。

## 快照身份与历史兼容

FilesystemSnapshotStore 原有文件信封已经以 4096 字节限制逻辑 `SnapshotId`。C6c2a 把同一上限前移到所有 StrongId 构造：

- 4096 字节 SnapshotId 仍可完成真实写、读、重启读取和删除；固定摘要文件名长度不变。
- 4097 字节新身份在 `SnapshotId::create()` 即失败，目标目录保持空，不再先构造后交给 Store 拒绝。
- 已落盘信封中伪造的 4097 字节身份仍由 Store 解码防线拒绝；Store 的 4096 检查保留为历史材料和内部防御，不因上游门禁删除。

其他既有持久表若包含旧二进制曾接纳的超长或畸形身份，新版本在解析为 StrongId 时失败关闭，不自动截断、改码、重命名或创建兼容别名。需要回退读取时必须配套旧二进制与成套旧数据归档；本阶段不增加直接 SQL 修复或恢复旁路。

## 公共契约变化

本节点只修改公共头 [strong_id.hpp](../../include/lasercnc/foundation/strong_id.hpp)，新增公开常量 `kernelStrongIdMaximumBytes` 和模板内部使用的严格 UTF-8 检查；`StrongId` 仍只保存一个 `std::string`，对象布局及比较/hash 的精确字节语义未变。

该头的 C6 起始 SHA-256 从 `6C1B8317EDD98026E562012759F16D4ED8DDC0BCBF96E78944C8B75D53CAAC25` 变为 `7D0927713717AC3E5887800E00D8402EC351F233A852E2245E009A450E78FEC2`。连同 C6c1 的 `value.hpp`，当前相对 C6 起始基线共有 2 个预期公共头摘要变化；其余 69 个保持不变。过程账本见 [公共头清单](Kernel-1.0-公共头清单.md#c6c2a-公共头增量)，最终冻结摘要仍只能在 C6–C8/ST1D 后签发。

## 红灯、回归与门禁

新增 Release 用例先在旧实现运行，4097 字节身份实际返回成功，`REQUIRE_FALSE(oversized)` 失败；该结果是本节点真实红灯。修复后同一用例覆盖 4096 精确边界及四类畸形 UTF-8 并转绿。

第一次 Release 全量中，498 项通过，既有 `Snapshot storage keys enforce identity and payload envelope budgets` 因测试辅助函数仍假定“4097 字节 SnapshotId 可先构造”而失败。生产行为是预期的更早拒绝，测试随后调整为检查 StrongId 错误和目录零副作用；这不是新的生产缺陷，也没有放宽门禁。最终证据：

- Release 全目标构建通过；完整回归 499/499，耗时 1032.65 秒。
- Debug 全目标构建通过；StrongId/快照预算焦点 2/2。
- ASan 全目标构建通过；相同 2 项连续三轮，共 6 次执行，无 sanitizer 报告。
- production-only Release 构建通过。
- 边界脚本通过 71 个公共头、141 个生产源及模块治理、ExecutionGateway 旁路、第三方实现目录隔离。

## 后续执行顺序

1. C6c2b：Command/Query/Task/Workflow/Script 注册描述与定义的 Schema、Value、文本、依赖/资源集合和跨字段累计预算；注册失败不得留下条目。
2. C6c2c：运行期请求/响应、DomainEvent、Trace/Metrics/Diagnostics/Log 的载荷、标签、消息和集合预算；拒绝必须早于 handler、排队或发布。
3. C6c2d：事务、History、Snapshot、Idempotency、Task/Effect/Workflow/Diagnostic 与恢复 DTO 的累计预算和写读双向终检；不得靠最终 JSON 序列化兜底全部入口。
4. C6d：同步/Task 取消与 deadline、活动和终态保留；之后 C7/C8/ST1D。

C6c2a 不签核注册表容量、同步抢占、终态保留、性能 SLA、跨工具链 ABI、设备安全或物理断电。C6c、C6 和 Kernel 1.0 均未 Frozen。
