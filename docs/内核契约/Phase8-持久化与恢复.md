# Phase 8 持久化与恢复契约

## 阶段状态

Phase 8 正在建设。目标是以 Snapshot、状态 Journal、SQLite Control Plane 和原子文件 Data Plane 建立可校验、可重放、可故障注入的 Crash Recovery。本阶段只处理 Application Kernel 持久化语义，不实现 Workflow、Script、CAD/CAM、GUI、设备控制或其他上层模块。

当前第一节点仅完成强内容摘要端口与 Windows Adapter；下述 Snapshot、Journal 和恢复规则是后续实现必须满足的准入约束，不代表相关代码已经完成。

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

## 尚未验收

- PersistenceService 与 schema migration；
- TransactionCommit 写前 Journal、幂等追加与读取；
- Snapshot codec、原子文件 store、SQLite snapshot index；
- DocumentStore 恢复装载和 Journal 重放；
- 持久幂等、Task/Diagnostics metadata；
- 校验和损坏、截断、断电点、数据库忙/只读/提交失败等故障注入；
- Debug/Release、重复、Production-only 与完整进程恢复门禁。
