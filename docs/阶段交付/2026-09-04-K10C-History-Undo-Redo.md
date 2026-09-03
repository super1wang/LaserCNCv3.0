# 2026-09-04 K10C History / Undo / Redo 交付

## 结论

K10C 已验收。Application Kernel 已闭合仅适用于 ApplicationTransaction 内 DocumentWrite 的普通 Undo/Redo：`HistoryEntry`、`HistoryCursor`、`UndoBarrier`、`edit.undo`、`edit.redo` 与 Journal v2 使用同一事务边界，正常重启后 Revision、对象状态和 History cursor 保持一致。

本交付只修改 Kernel、Persistence、自动化测试与中文文档。没有创建或扩展 CAD、CAM、Machine、Process、Collision、Qt GUI、产品 CLI/RPC、AI 等上层模块。K10D–K10F 尚未完成，因此 Kernel 1.0 仍未 Frozen。

## 交付内容

1. AppKernel 所有的 `HistoryRuntime` 与只读 `DocumentHistorySnapshot`；
2. `HistoryEntry`、`HistoryCursor`、`UndoBarrier` 及稳定 HistoryEntryId；
3. 仅同步 Document Scope / DocumentWrite 接受 `undoable=true` 的注册规则；
4. 内建 `edit.undo@1.0.0`、`edit.redo@1.0.0` 与 `kernel.history.edit` Capability；
5. Created、Updated、Removed 的严格反向/正向变换和对象材料前置校验；
6. Undo/Redo 新事务的 Revision 单调推进、redo 分支替换和非可撤销写屏障；
7. `lasercnc.state-journal` payload v2 内的 Record/Barrier/Undo/Redo mutation；
8. Journal、History、Revision、同步幂等结果的一次原子持久化；
9. 完整 Journal sequence 重建 cursor/redo/barrier，以及旧 v1 有效提交的屏障兼容；
10. History 语义篡改、cursor/target 漂移和对象状态漂移的 fail-closed 结果。

## 自动化证据

环境：Windows、Visual Studio 2022、x64、MSVC 19.44.35216、Windows SDK 10.0.26100.0。

```powershell
cmake --build --preset vs2022-debug --parallel 1 -- /nodeReuse:false
ctest --preset vs2022-debug --output-on-failure

cmake --build --preset vs2022-release --parallel 1 -- /nodeReuse:false
ctest --preset vs2022-release --output-on-failure

# Debug 全集连续执行 20 轮
1..20 | ForEach-Object {
  ctest --preset vs2022-debug --output-on-failure
  if ($LASTEXITCODE -ne 0) { throw "Debug CTest repeat $_ failed" }
}

cmake -S . -B build/production-only -G "Visual Studio 17 2022" -A x64 `
  -DLCNC_BUILD_TESTING=OFF -DLCNC_WARNINGS_AS_ERRORS=ON
cmake --build build/production-only --config Release --parallel 1 -- /nodeReuse:false

cmake -DLCNC_SOURCE_ROOT=J:/Code/LaserCNCv3.0 `
  -P cmake/VerifyKernelBoundaries.cmake
git diff --check
```

最终结果：

- Debug：161/161 CTest 通过；
- Release：161/161 CTest 通过；
- Debug 全集连续 20 轮通过，共 3,220 项测试；
- Production-only Release：构建通过，31 个 VS 工程中测试/contract/Catch2 目标为 0，Catch2 source 不存在，CTest 文件为 0；
- 架构扫描：62 个 Kernel 公共头文件、120 个生产源文件通过；
- `git diff --check` 通过。

## 覆盖重点

- `undoable=true` 的合法 DocumentWrite 与 ReadOnly/异步/外部副作用拒绝；
- `edit.undo` / `edit.redo` 继续经过 CommandRuntime 和 Capability；
- Create → Undo → Redo；Update → Undo → Redo；Remove → Undo → Redo；
- 连续条目、撤销后新提交替换 redo 分支、非可撤销写形成 UndoBarrier；
- Undo/Redo 的对象完整材料比对和 Revision 单调推进；
- Journal 插入失败时对象、Revision、HistoryEntry 与 cursor 均保持不变；
- AppKernel 正常重启恢复条目、cursor 与 redo 对象材料；
- 同一持久幂等请求重启重放不重复建立条目、不移动 cursor，并返回原 Commit History mutation；
- History payload 被改成字段形状冲突后，即使攻击者重新计算有效 SHA-256 摘要，恢复仍 fail-closed；
- History payload 增加未知字段后，即使攻击者重新计算有效 SHA-256 摘要，恢复仍 fail-closed；
- 可撤销与不可撤销 DocumentWrite 的空事务均按既有契约拒绝，不产生无意义 HistoryEntry 或 UndoBarrier；
- v1 Journal 的有效历史写入按 UndoBarrier 恢复，不猜测为可撤销条目；
- 既有独立进程 Persistence/Workflow 恢复门禁和全部历史回归继续通过。

## 证据边界与后续

本阶段证明正常 AppKernel 重启下的 History 确定恢复，并继续通过已有独立进程 Persistence/Workflow 恢复门禁；没有新增“在 Undo/Redo Journal 写入各指令点强制 `_Exit`”的崩溃矩阵。History 长链性能、容量/裁剪策略、并发压力、故障注入矩阵与长稳态属于 K10F，不能由本阶段单元与组件测试替代。

下一阶段严格进入 K10D Module Governance / Execution Gateway。本阶段没有实现 ModuleRegistrar、ExecutionGateway、ObjectType/Asset Registry、动态插件加载器或任何上层业务能力。
