# 2026-09-04 K10A Execution Contract 1.0 交付

## 结论

K10A 已验收。Command/Query 的版本身份、四级 Execution Scope、ReadOnly/DocumentWrite/External Effect 三类执行路径、外部副作用持久恢复策略、Effect Guard 与统一 ResourceManager 租约已经闭环。

本交付只修改 Application Kernel、Infrastructure 持久化适配、自动化测试和中文文档，没有创建或扩展 CAD、CAM、Machine、Process、Collision、控制器、Qt GUI、产品 CLI/RPC、AI 或其他上层模块。K10B–K10F 尚未完成，因此 Kernel 1.0 仍未 Frozen。

## 交付内容

1. 版本化执行身份
   - CommandKey/QueryKey 由名称与 Version 共同组成，同名多版本可共存；
   - Exact 只接受完全相等版本；Compatible 只在同 major 中选择不低于请求的最高版本；
   - Deprecated 可发现、可执行并随 Response 返回；NotFound 与 UnsupportedVersion 分离；
   - Workflow/Script 在 Freeze 时按精确版本检查，持久 Command 签名包含请求/解析版本和解析策略。
2. ExecutionContext 与 Scope
   - SessionId 始终存在，ProjectId/DocumentId 按 Global、Session、Project、Document 严格必填或禁止；
   - Scope 在 Schema、Capability、Revision 和 handler 前校验；
   - 只有 Document Scope Query/ReadOnly/Effect 可以取得不可变 Document Snapshot，并复核 Project ownership。
3. 三类命令执行路径
   - 同步 ReadOnly 使用 IReadOnlyCommandHandler，不创建事务、不暴露可变 Document；
   - DocumentWrite 保持 ICommandHandler → ApplicationTransaction 唯一写入链；
   - 外部副作用使用 IExternalEffectHandler → EffectExecutor，不得借用事务或普通 Undo；
   - Registry 双向拒绝 handler、ExecutionMode、SideEffect 与外部元数据错配。
4. External Effect 持久状态机
   - SQLite schema v7 新增 external_effects，保存强摘要签名、ReplayPolicy、状态与结果；
   - durable claim 在 handler 前提交；Completed 只重放摘要与 Schema 均有效的结果；
   - Safe/Idempotent 失败或崩溃后为 Interrupted，只允许同键同签名显式重试；
   - ReconcileOnly 为 ReconcileRequired，Never 为 Indeterminate，均禁止直接重试；
   - 初始化只归一遗留 Executing，不自动调用 handler，状态/策略不一致或摘要损坏 fail-closed。
5. Effect Guard 与 ResourceManager
   - IEffectGuard 由稳定 EffectGuardId 注册，Ready 时冻结；
   - 执行顺序为 Capability → Preconditions → Guard → Resource lease → durable claim → handler；
   - 缺失/重复 Guard 或缺失持久化阻止 Kernel Ready；
   - Guard 拒绝和资源冲突发生在 claim/handler 前，所有返回路径释放租约；
   - Kernel 只提供通用 Guard，不实现碰撞、互锁、运动许可或激光安全领域逻辑。

## 自动化证据

环境：Windows、Visual Studio 2022、x64、MSVC 19.44.35216、Windows SDK 10.0.26100.0。

```powershell
cmake --build --preset vs2022-debug --parallel 1
ctest --preset vs2022-debug --output-on-failure

cmake --build --preset vs2022-release --parallel 1
ctest --preset vs2022-release --output-on-failure

ctest --preset vs2022-debug --repeat until-fail:20 --output-on-failure

cmake -S . -B build/production-only -G "Visual Studio 17 2022" -A x64 `
  -DLCNC_BUILD_TESTING=OFF -DLCNC_WARNINGS_AS_ERRORS=ON
cmake --build build/production-only --config Release --parallel 1

cmake -DLCNC_SOURCE_ROOT=J:/Code/LaserCNCv3.0 `
  -P cmake/VerifyKernelBoundaries.cmake
git diff --check
```

最终结果：

- Debug：145/145 CTest 通过；
- Release：145/145 CTest 通过；
- Debug repeat：145 项连续 20 轮通过，共 2,900 次测试执行，175.33 秒；
- Production-only Release：构建通过，31 个 VS 工程中测试/contract/Catch2 目标为 0，Catch2 source 不存在，CTest 文件为 0；
- 架构扫描通过 60 个 Kernel 公共头文件和 115 个生产源文件；
- `git diff --check` 通过。

## 覆盖重点

- Exact/Compatible/Deprecated/Unsupported 版本行为及幂等签名漂移；
- Global/Session/Project/Document Scope 与 handler 前拒绝；
- 同步 ReadOnly 无事务执行和 Document 不可变快照；
- External Effect 完成重放、跨 PersistenceService 重开恢复、Safe 显式重试；
- ReconcileOnly/Never 失败后的 ReconcileRequired/Indeterminate 与直接重试拒绝；
- Capability/Guard 拒绝、缺失依赖启动失败、独占资源并发冲突；
- ReplayPolicy/状态不一致、签名冲突、结果摘要与 Schema 校验；
- 既有 Journal、Snapshot、Task、Diagnostics、Workflow、Script 与独立进程恢复回归。

## 证据边界与后续

K10A 使用真实 SQLite 文件的关闭/重开验证外部执行状态恢复，并证明重开本身不调用 handler。规划要求的真正独立进程 external effect crash 测试属于 K10F，将与 command/history/asset crash 一起纳入最终可靠性认证。

本交付没有实现任何真实文件发布、网络、Machine、Motion 或 Laser handler，也没有替代后续 Machine/Safety Domain 的 Guard；因此不构成控制器、运动、激光或物理安全验收。下一阶段严格进入 K10B Runtime Project/Document Lifecycle。
