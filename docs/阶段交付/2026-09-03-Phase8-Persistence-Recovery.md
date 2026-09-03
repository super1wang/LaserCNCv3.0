# 2026-09-03 Phase 8 持久化与崩溃恢复交付

## 结论

Phase 8 已验收。Kernel 已建立以 SQLite 为 Control Plane、原子文件为 Data Plane 的持久化与恢复边界：ApplicationTransaction 在内存状态替换前持久化 Journal，Snapshot 使用强摘要与不可变身份，AppKernel 在进入 Ready 前恢复状态；Command 幂等、Task History 与 Diagnostics Metadata 均可跨进程重启读取。本阶段没有新增 Workflow、Script、CAD、CAM、Machine、Process、Qt GUI、产品 CLI/RPC、AI 或其他上层模块。

## 交付内容

1. 内容身份与文件 Data Plane
   - 新增 SnapshotId、ContentDigest 与 IHashService，Kernel 公共 API 不暴露 Windows 或第三方类型；
   - Windows CNG Adapter 生成 `sha256:<64 位小写十六进制>` 强摘要；
   - FilesystemSnapshotStore 通过临时文件、flush 和原子替换发布 Snapshot，限制文件名与 payload 大小；
   - 相同 SnapshotId/相同内容可幂等重试，不同内容 fail-closed，索引只在文件发布成功后提交。
2. SQLite Control Plane
   - PersistenceService 独占持久化编排，领域代码不散布 SQL；
   - v1 至 v5 migration 依次建立 `state_journal`、`snapshot_index`、`command_idempotency`、`task_history` 与 `diagnostic_history`；
   - payload 均采用版本化序列化和 SHA-256 摘要，读取时重新校验内容、身份与控制面元数据；
   - 未知的更高 schema 版本、摘要损坏、身份冲突和 Revision 不一致均拒绝继续运行。
3. Journal 与应用事务
   - Journal 保存 Transaction、Project、Document、六域 Revision、ObjectChange 和已提交 Domain Event 材料；
   - TransactionManager 用独立 commit mutex 串行化提交，在 Document 锁外完成 serializer/hash/SQLite I/O；
   - Journal 成功后才以无失败 swap 更新内存，Journal 失败不会提升 Revision、改变对象或发布 Event；
   - 同一 TransactionId/相同内容返回原 sequence，不同内容拒绝重绑。
4. Snapshot 与 Crash Recovery
   - Snapshot 记录明确 Document Revision、ProjectRevision 和全局 Journal 水位；
   - 恢复在一致读事务中校验全局 sequence、项目/文档 Revision 链、Snapshot 文件大小与摘要；
   - Snapshot 后只重放 ObjectChange，不调用 Command/Task handler，也不重新投递历史 Event；
   - 恢复出的 DocumentImage 在 AppKernel Ready 前原子装入 DocumentStore，任一损坏或冲突均阻止 Ready。
5. 持久 Command 幂等
   - 以 Command/版本/参数/上下文/期望 Revision 构造稳定请求签名；
   - 同步 Command 的 Journal 与结果、异步 Command 的 Task 接受与结果分别在一个 SQLite 事务中提交；
   - 重启重试返回原 TransactionCommit 或 TaskId，不重新调用 handler、不重复发布 Event 或提交 Task；
   - 遗留 pending claim 在单 Host 启动模型下标记为 abandoned，同签名请求可以重新取得。
6. Task History
   - Task 只有在持久接受提交后才进入可调度态；请求保存输入、依赖、资源、上下文、Revision 与 deadline 身份；
   - Succeeded、Failed、Cancelled、Stale 终态不可变并保存结果或 Error 材料；
   - 重启时 accepted/running 任务明确变为 `Task.InterruptedByRestart`，不会自动重跑；
   - 终态持久化失败不反转业务终态，并进入 Scheduler 有界失败缓存。
7. Diagnostics Metadata
   - DiagnosticsService 先更新本地 latest，再在锁外调用 exporter；
   - PersistenceService exporter 将历史追加到 SQLite，支持顺序历史、每项 latest 与重启读取；
   - 写入失败只进入 exporter failure 缓存，不改变 DiagnosticReport 或触发恢复动作。

## 独立进程恢复门禁

CTest 使用两个串行独立进程验证真实进程边界：

```text
播种进程
  -> 写 Snapshot
  -> 写 Journal tail
  -> 完成持久 Command 幂等结果
  -> 接受未完成 Task
  -> 写 Diagnostics
  -> std::_Exit 跳过析构

恢复进程
  -> migration + recover
  -> 校验 Snapshot + Journal tail
  -> 返回原 Command Commit，handler/Event 计数保持 0
  -> 未完成 Task 读取为 InterruptedByRestart
  -> 读取 Diagnostics 历史
```

该门禁不会依赖同一进程内的内存对象或正常析构，证明恢复材料确实来自持久介质。

## 自动化证据

环境：Windows、Visual Studio 2022、x64、MSVC 19.44.35216、Windows SDK 10.0.26100.0。

```powershell
cmake --build --preset vs2022-debug --parallel 16
ctest --preset vs2022-debug --output-on-failure

cmake --build --preset vs2022-release --parallel 16
ctest --preset vs2022-release --output-on-failure

ctest --preset vs2022-debug --repeat until-fail:20 --output-on-failure

cmake -S . -B build/production-only -G "Visual Studio 17 2022" -A x64 `
  -DLCNC_BUILD_TESTING=OFF -DLCNC_WARNINGS_AS_ERRORS=ON
cmake --build build/production-only --config Release --parallel 16

cmake -DLCNC_SOURCE_ROOT=J:/Code/LaserCNCv3.0 `
  -P cmake/VerifyKernelBoundaries.cmake
```

结果：

- Debug：109/109 CTest 通过；
- Release：109/109 CTest 通过；
- Debug repeat：109 项连续 20 轮通过，共 2180 次测试执行，101.50 秒；
- 独立进程恢复测试同时包含在 Debug、Release 与 20 轮重复门禁中；
- Production-only Release：构建通过，31 个生成的 VS project 中测试/contract/Catch 类目标为 0，`_deps/catch2-src` 不存在；
- `git diff --check` 通过；
- 架构扫描检查 51 个 Kernel 公共头文件和 95 个生产源文件，未发现第三方类型或上层模块越界。

## 覆盖重点

- migration 幂等与未知新版本拒绝；
- Journal 顺序、TransactionId 幂等、冲突重绑、摘要/元数据篡改和写前内存原子性；
- Snapshot 原子发布、不可变身份、路径/大小约束、同项目多文档全局水位；
- Snapshot + Journal tail、纯 Journal、对象 before/after、Revision 链和 DocumentStore 原子恢复；
- 同步/异步 Command 跨重启幂等及无 handler/Event 重放；
- Task 原子接受、不可变终态、重启中断、摘要篡改与终态写失败可见性；
- Diagnostics 内存优先、SQLite 历史/latest、重启读取、篡改闭锁和写失败隔离；
- backend/serializer/hash/回滚失败、文件缺失/截断/损坏、Journal sequence 缺口与独立进程异常退出。

## 已知边界与后续

本交付只支持一个活动 AppKernel Host 独占一个数据库。跨进程 lease、多 Host 共享写入、数据库外部 busy 竞争和只读部署目录没有作为已通过能力宣称；它们需要在出现真实部署需求时单独建立故障矩阵。当前恢复只重建可信内核状态，不恢复 handler 执行栈、不自动重跑中断 Task，也不会重放文件发布、设备控制、运动、激光或其他外部副作用。

下一阶段严格进入 Phase 9：Workflow Runtime 与 Script Runtime。CAD/CAM/GUI/RPC/AI、控制器 SDK、真实设备与物理机验收均继续延后。
