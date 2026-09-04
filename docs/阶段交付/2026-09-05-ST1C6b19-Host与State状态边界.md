# ST1C6b19 Host 与 State 状态边界交付

## 范围与结论

本检查点只修改 Application Kernel 测试、契约账本和中文执行计划，不改生产实现、公共 API、SQLite schema 或持久格式。契约见 [C6b19](../内核契约/ST1C6b19-Host与State状态边界.md)。

- AppKernel/Module、Project/Document Runtime 和 PersistenceOwnership 的状态是内核产生的只读观察结果，不新增任意整数写入口。
- RevisionScope 与 ObjectPersistencePolicy 是实际闭集输入，既有 `RevisionManager` / `ObjectTypeRegistry` 负例确认未定义值按 Result 拒绝。
- `RevisionSet::at()` 是只接受已验证 scope 的 `noexcept` 直接访问器，不是反序列化入口；本节点没有通过默认零值掩盖调用错误。
- Project/Document persistence state 是实际公开写入材料。Project 未定义状态已有事务前拒绝回归；本节点补齐 Document 的 6/255 未定义值拒绝，并用独立 SQLite 连接证明既有目录行逐字段不变。
- Project 0:N Document，两个 Runtime 目录和两个 lifecycle v1 记录分别持有状态事实；Opening/Closing 在恢复观察时映射为 Failed + interruptedTransition，但读取不改写历史证据。

## 缺口证据

源码审计发现的是测试与集中契约缺口，不是生产行为缺陷。因此本节点没有伪造“修复前红灯”：新增回归在当前生产实现上首次运行即通过。

新增 2 个用例、34 条断言：

1. Project/Document 生命周期名称覆盖全部合法闭集，两个未定义值均只返回 `unknown` 诊断；
2. Document lifecycle v1 写入口拒绝 6 和 255，返回 `Persistence.InvalidDocumentLifecycleState`，拒绝前后的 `document_catalog` 全行完全相等。

已有 9 个相关用例共同覆盖 AppKernel/Module 只读状态与失败回滚、Revision 六个 scope/未知值/重复/溢出、对象持久策略未定义值、Project 未定义持久状态及 Document 生命周期恢复/篡改。定向矩阵合计 11 项。

## 最终门禁

- Debug/Release 构建通过；两配置 Host/State 定向矩阵各 11/11。
- ASan 构建通过；同一 11 项矩阵连续三轮，共 33 次执行通过，无 sanitizer 报告。
- Release 全集：486/486，通过，1082.58 秒；包含压力、故障注入、独立进程恢复、架构边界和 Headless 往返。
- C6b19 新增回归：2/2、34 条断言通过。
- 纯生产 Release 构建通过；边界门禁检查 71 个公共头、141 个生产源，并通过模块治理、执行网关旁路与第三方实现目录隔离检查。

本节点不是 C6 大节点，不推送远端。当前计划已收紧为：下一步 C6b20 审计 Idempotency、ExternalEffect、Diagnostic、恢复/会话 DTO 和剩余持久版本；随后对账 71 个公共头，再进入 C6c 统一预算和 C6d 同步/Task/有界终态。

## 可复现命令

```powershell
cmake --build --preset vs2022-debug --parallel 16
cmake --build --preset vs2022-release --parallel 16
cmake --build --preset vs2022-asan --parallel 16
ctest --preset vs2022-debug -R "AppKernel state observers|ModuleRuntime uses deterministic dependency lifecycle ordering|ModuleRuntime rolls back services and modules after start failure|Revision models all kernel consistency scopes|Revision validates conflicts duplicates and overflow|ObjectTypeRegistry rejects ambiguous and incomplete migration contracts|Project catalog validates states and reports interrupted transitions without rewriting|Document lifecycle catalog survives close open remove and restart|Document lifecycle catalog rejects ownership drift and tampering|Lifecycle state names preserve closed sets and mark undefined values|Document lifecycle persistence rejects undefined states before mutation"
ctest --preset vs2022-release -R "AppKernel state observers|ModuleRuntime uses deterministic dependency lifecycle ordering|ModuleRuntime rolls back services and modules after start failure|Revision models all kernel consistency scopes|Revision validates conflicts duplicates and overflow|ObjectTypeRegistry rejects ambiguous and incomplete migration contracts|Project catalog validates states and reports interrupted transitions without rewriting|Document lifecycle catalog survives close open remove and restart|Document lifecycle catalog rejects ownership drift and tampering|Lifecycle state names preserve closed sets and mark undefined values|Document lifecycle persistence rejects undefined states before mutation"
ctest --preset vs2022-asan --repeat until-fail:3 -R "AppKernel state observers|ModuleRuntime uses deterministic dependency lifecycle ordering|ModuleRuntime rolls back services and modules after start failure|Revision models all kernel consistency scopes|Revision validates conflicts duplicates and overflow|ObjectTypeRegistry rejects ambiguous and incomplete migration contracts|Project catalog validates states and reports interrupted transitions without rewriting|Document lifecycle catalog survives close open remove and restart|Document lifecycle catalog rejects ownership drift and tampering|Lifecycle state names preserve closed sets and mark undefined values|Document lifecycle persistence rejects undefined states before mutation"
ctest --preset vs2022-release
cmake --build build/production-only --config Release --parallel 1 -- /nodeReuse:false
cmake -DLCNC_SOURCE_ROOT=J:/Code/LaserCNCv3.0 -P cmake/VerifyKernelBoundaries.cmake
```

## 未完成范围

本节点不代签其他 Host/执行 API、剩余 Persistence DTO/格式、Value/字符串/集合预算、同步取消/deadline、终态保留、容量、跨工具链 C++ ABI 或远端策略。C6b/c/d、C7/C8、ST1D 与整体 Kernel Frozen 仍未完成。
