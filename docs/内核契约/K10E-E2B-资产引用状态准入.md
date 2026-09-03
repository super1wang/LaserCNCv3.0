# K10E E2B 资产引用状态准入

## 范围

本节点把 E2A 的不可变资产端口接入 ObjectRecord、ApplicationTransaction、Document 生命周期、History、Journal、Snapshot 与幂等回执。只实现内核元数据和准入，不解析任何几何、网格、刀路或控制器格式，不新增三方库。

## 引用与存储所有权

`ObjectRecord::assets` 是显式 `AssetRef` 列表，位于 `schemaVersion` 之后。Kernel 不通过扫描普通 Value 的字符串猜测资产引用；大载荷仍在 Data Plane，Value 只放小型元数据。同一对象内禁止重复 AssetId，不同对象可共享完全相等的引用；同一次准入内相同身份只能对应同一套摘要、Kind 和长度。

Host 只能在 Configuring 阶段调用一次 `AppKernel::configureAssetStore()` 注入非空共享 Store。Kernel 持有其生命周期，不提供可变 Store getter；带引用的状态必须有 Store，没有资产的既有文档不强制配置 Store。Store 实现必须遵循不可变与并发只读验证契约，不能用永远成功的验证器作为生产实现。

先发布并验证资产，再通过 Command/ApplicationTransaction 写入引用。`replaceObjectAssets()` 只替换资产列表；普通数据替换和显式 Schema 迁移保留原列表。发布成功但事务失败允许留下孤立资产，不提供自动垃圾回收、覆盖或删除。

## 准入位置与失败语义

1. 事务先验证完整候选对象类型和引用图，再验证候选资产及本次 change set 的所有 before-image 资产，最后才获取提交锁、持久化并安装状态。删除或清空已损坏引用不能把悬空材料写入 Journal/History。
2. Store 校验在提交锁和活动 Document 锁外执行；同一批次的完全相等引用只验证一次。异常转换为 `Asset.StateAdmissionException`，底层失败带 cause 返回 `Asset.StateAdmissionFailed`；失败不改变 Document、Revision、Journal 或 History。
3. attach/open 在安装对象和更新生命周期之前验证资产。持久化 close 在写 Snapshot 前再次验证当前对象引用；失败保留文档，按既有 Closing 失败规则进入 Failed。
4. 启动在所有模块注册后、initialize/start 前，对恢复的当前文档及保留 History 的 before/after 对象验证资产。准入失败不安装恢复文档，并回滚模块贡献；不存在“先 Ready 后补验证”。
5. Undo/Redo 经原有 CommandRuntime 和 TransactionManager，因此恢复历史引用也必须通过相同准入。资产列表是完整对象相等性与历史材料的一部分。

缺少 Store 返回 `Asset.StoreRequired`；同对象重复引用返回 `Asset.DuplicateReference`；跨记录身份元数据冲突返回 `Asset.ReferenceConflict`。恢复外层错误保留已有 ObjectType 恢复准入错误码，其 cause 包含资产错误。

## 持久化版本

| 载体 | 旧无版本对象 | 精确版本对象 | 当前资产对象 |
| --- | --- | --- | --- |
| Journal | v1/v2 | v3 | v4 |
| Snapshot | v1 | v2 | v3 |
| Command outcome | v1 | v2 | v3 |

旧无版本对象严格为 id/type/data，映射 schemaVersion=1.0.0、assets 为空。精确版本旧对象严格为四字段，保留其 schemaVersion，assets 为空。新对象严格为五字段，必须包含 assets 数组（允许空），每项严格包含 id/digest/kind/byteSize。

byteSize 编码为规范无符号十进制字符串，覆盖 uint64 全范围；负数、前导零、溢出、数值型 Value、缺字段和额外字段均拒绝。旧格式不根据当前注册版本推断或补猜资产。

幂等重放返回原提交中的完整资产引用，且不重复 handler、不改状态、不移动历史游标。回执表示当时的结果，不是此刻文件可用性证明；使用内容仍必须经 Store 的 read/verify。与 Journal 中原始提交不一致的回执继续由既有一致性检查拒绝。

## 证据与限制

自动化覆盖真实文件 Store 的创建/替换、close/open、跨重启、幂等重放、Undo/Redo，未配置 Store、字段伪造、文件损坏、历史独占资产损坏、提交前像损坏、关闭失败保留状态，以及验证器锁外读重入和异常隔离。编解码测试覆盖 uint64 最大值与非法 wire 形状；旧 v1/v3 Journal 有显式恢复测试。

摘要是完整性校验，不是来源认证。接口内不可变是准入后仍有效的前提；进程外文件删除或篡改在后续读/提交/open/恢复时检测，不承诺持续监控或阻止管理员写入。当前验证为有界整块读取，性能取舍留待 K10F 基线。独立进程 Asset publish crash 仍须在 K10F2 证明，不能用同进程重建 Store 或错误返回代替。

最终门禁见 [K10E 交付记录](../阶段交付/2026-09-04-K10E-ObjectType-Asset-Boundary.md)。
