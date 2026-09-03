# 2026-09-04 K10B Runtime Project / Document Lifecycle 交付

## 结论

K10B 已验收。Application Kernel 已具备运行期文档创建、装载、快照、关闭、卸载、移除与枚举能力，并用统一准入闸门和 SQLite v8 强摘要目录闭合关闭竞态与重启恢复。

本交付只修改 Kernel、Persistence、自动化测试与中文文档。没有创建或扩展 CAD、CAM、Machine、Process、Collision、Qt GUI、产品 CLI/RPC、AI 等上层模块。K10C–K10F 尚未完成，因此 Kernel 1.0 仍未 Frozen。

## 交付内容

1. AppKernel 所有的 DocumentRuntime 与明确状态机；
2. create、attach/open、snapshot、close、detach、remove、lifecycle/list；
3. Command/Query/Transaction/Task/Workflow/Script 共用文档活动准入；
4. Transaction、Task、Workflow、Script 按 Document 的活动阻塞；
5. SQLite schema v8 `document_catalog`、强摘要 payload、ownership 校验与中断状态恢复；
6. 只恢复 Open、Detached 显式 open、Removed tombstone 防历史状态复活；
7. close 的 Snapshot → Detached → 内存卸载顺序，以及持久化失败的 Failed 状态；
8. 持久 Workflow 恢复前的 Document Open 与 Project ownership 校验。

## 自动化证据

环境：Windows、Visual Studio 2022、x64、MSVC 19.44.35216、Windows SDK 10.0.26100.0。

```powershell
cmake --build --preset vs2022-debug --parallel 16
ctest --preset vs2022-debug --output-on-failure

cmake --build --preset vs2022-release --parallel 16
ctest --preset vs2022-release --output-on-failure

cmake -S . -B build/production-only -G "Visual Studio 17 2022" -A x64 `
  -DLCNC_BUILD_TESTING=OFF -DLCNC_WARNINGS_AS_ERRORS=ON
cmake --build build/production-only --config Release --parallel 16

cmake -DLCNC_SOURCE_ROOT=J:/Code/LaserCNCv3.0 `
  -P cmake/VerifyKernelBoundaries.cmake
git diff --check
```

最终结果：

- Debug：154/154 CTest 通过；
- Release：154/154 CTest 通过；
- Production-only Release：构建通过，31 个 VS 工程中测试/contract/Catch2 目标为 0，Catch2 source 不存在，CTest 文件为 0；
- 架构扫描：61 个 Kernel 公共头文件、118 个生产源文件通过；
- `git diff --check` 通过。

## 覆盖重点

- 运行期 create/attach/open/snapshot/close/detach/remove/list；
- 非 Ready 状态拒绝与重复加载；
- Document close 与 Query 并发、非终态 Task、Workflow、Script；
- Transaction 按 Document 的活动统计；
- 空文档目录同步及异步 Command/Task 重启重放回归；
- Detached 重启不自动加载、显式 open 恢复、Removed 重启不复活；
- Snapshot Store 缺失时文档保留且进入 Failed；
- Project ownership 漂移、目录状态/摘要篡改与后续覆盖均 fail-closed；
- 持久 Workflow 定义漂移优先诊断，以及文档不可用拒绝。

## 证据边界与后续

本阶段使用真实 SQLite 文件与文件系统 Snapshot Store 完成同进程关闭/重开及 AppKernel 重启恢复。K10F 要求的独立进程 document-close 崩溃点、close 与更多执行路径压力竞态、长稳态和性能基线尚未执行，不能提前宣称最终可靠性认证。

下一阶段严格进入 K10C History / Undo / Redo。本阶段没有实现 HistoryEntry、Undo/Redo Command、ModuleRegistrar、ExecutionGateway、ObjectType/Asset 或任何上层业务能力。
