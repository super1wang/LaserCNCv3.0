# K10E E2A 不可变资产存储基础组件

## 状态

本节点实现 Data Plane 最小类型与文件存储组件，供后续内核对象引用准入使用。尚未在 ObjectRecord/Journal/History 中接入 AssetRef，也未宣称 Document 中的资产悬空引用已经被阻止。K10E 与 Kernel 1.0 仍未验收。

没有接入 OCCT、网格、刀路、控制器或任何上层模块；继续复用现有 SHA-256 与 Windows 文件基础设施，无新增第三方依赖。

## 最小端口

- `AssetId`、`AssetKind` 为独立 StrongId，不能与 ObjectId/ContentDigest 混用。
- `AssetRef` 保存 AssetId、ContentDigest、AssetKind 与 uint64 字节长度，不承载二进制内容。
- `IAssetStore::publish(kind, bytes)` 返回已经验证的不可变引用。
- `read(ref)` 返回经过完整性校验的字节数组；`verify(ref)` 校验同一组身份、元数据和内容，不返回内容副本。
- 不提供覆盖、删除、可变文件句柄或垃圾回收端口。业务状态依然只能由后续 ApplicationTransaction 记录引用。

## 文件适配器

`FilesystemAssetStore` 要求独立目录、正的内容大小上限和明确注入的 IHashService。当前文件格式固定使用规范的 `sha256:` 加 64 位小写十六进制摘要；摘要实现必须支持并发只读调用。

ContentDigest 是原始二进制内容的摘要。AssetId 是规范元数据串的摘要，绑定 Kind、ContentDigest 与字节长度，因此：

- 相同 Kind 和内容得到相同引用，可重复发布；
- 相同内容、不同 Kind 可并存，内容摘要相同但 AssetId 不同；
- 引用的 id/digest/kind/size 任何一项与实际文件不一致都拒绝；
- Kind 只是格式契约标识，文件适配器不解析或证明几何、刀路等领域格式正确性。

身份元数据串为 `LCNCAssetRef1\n`、Kind 字节长度的十进制表示、冒号、Kind、换行、内容摘要、冒号、内容字节长度十进制表示。身份格式前缀为 `asset.sha256.`。

内部文件封装为 `LCNCAS01` 魔数、4 字节大端 Kind 长度、Kind 字节和原始内容；Kind 上限 256 字节，默认内容上限 512 MiB。当前实现是有界整块读写，不包含流式、内存映射或零拷贝能力；不得用此节点代替后续性能基线。

## 不可变发布与失败

复用 Windows FilesystemSnapshotStore 的临时文件、完整写入、FlushFileBuffers 与不覆盖目标的原子重命名。Asset 目录中的 `.snapshot` 后缀只是内部不可变文件机制的复用，不会将 Data Plane 载荷写进 SQLite 或应用文档 Snapshot。

临时文件序号已改为进程级原子计数，避免同进程多个存储实例在同一时钟粒度下生成相同临时路径。8 个独立实例并发发布同一资产最终只留下一个完整文件。

发布前计算内容与身份摘要；发布后重新读取并验证文件。若发布后验证失败，调用方得到失败而非 AssetRef，文件可以作为孤立资产保留；修复哈希服务后可以再次验证复用。不得为“自动修复”覆盖或删除已有不同内容。

缺失、截断、错魔数、引用篡改、同长度内容损坏均 fail-closed。哈希错误、异常、大小超限、无法初始化目录和发布失败均返回 Error。

本接口的不可变性以受管理 Store 内无外部篡改为前提。管理员删除或修改文件后，下次读取、验证和后续恢复准入必须报错；不承诺阻止进程外恶意写入，也不把摘要当成来源认证。

## 验证

- Debug 全量 192/192 CTest 通过。
- 新增 6 个资产组件用例重复 20 次，共 120 项通过；并发测试每轮启动 8 个独立文件存储实例。
- 覆盖空内容/含 NUL 二进制往返、跨实例和重建 Store 后读取、同内容不同 Kind、规范摘要、字段伪造、文件缺失/截断/等长损坏、边界大小、哈希错误/异常/畸形输出、发布后孤立文件复用，以及目标目录冲突时不覆盖既有文件系统状态。
- 架构扫描：68 个公共头文件、131 个生产源文件通过。
- Release 全量 192/192 CTest；Debug/Release 警告错误门禁构建通过。

独立进程 Asset publish crash、ObjectRecord 引用持久化、Commit/open/recovery/History 资产准入与 K10F 故障矩阵仍是后续节点，不能由本组件测试替代。
