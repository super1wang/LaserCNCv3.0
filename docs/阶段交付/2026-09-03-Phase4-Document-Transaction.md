# 2026-09-03 Phase 4 Document 与 Application Transaction 交付

## 结论

Phase 4 已验收。Document、ObjectRegistry、Revision 与 ApplicationTransaction 已形成不可绕过的 Kernel 状态写入边界；原子提交、乐观冲突检测和提交后事件规则均有自动化验证。本阶段没有扩展 CAD、CAM、GUI、CommandRuntime、EventBus、TaskRuntime 或持久化编排。

## 交付内容

1. 文档与对象状态
   - DocumentStore 独占活动文档可变状态，对外只返回按值不可变快照；
   - ObjectRegistry 只公开稳定 ObjectId 查询和确定性枚举，写入口仅对事务内部开放；
   - ProjectId、DocumentId、ObjectId、ObjectTypeId、TransactionId 类型隔离。
2. 修订一致性
   - RevisionSet 覆盖 Project、Document、Geometry、CAM、MachineContext、Environment；
   - ProjectRevision 在项目内跨文档共享，当前采用保守的项目级冲突粒度；
   - begin 支持显式前置条件，commit 校验事务开始时捕获的全部六类 Revision；
   - 重复前置条件、非法 scope 和溢出均 fail-closed；推进集合中的重复 scope 只递增一次，失败不会修改原 RevisionSet。
3. 应用事务
   - copy-on-write staging state 隔离未提交写入；
   - 任一写失败使事务中毒，禁止部分提交；
   - commit 在独占锁内完成冲突校验，并在全部可失败材料准备完毕后原子 swap；
   - rollback、析构放弃、空提交、文档消失和并发失败不改变活动文档；
   - change set 为创建、更新、删除保存 before/after，供后续 Undo/Journal 使用。
4. 提交后事件
   - PendingDomainEvent 只存在于事务私有集合；
   - CommittedDomainEvent 构造器保持私有，只能由成功提交路径创建；
   - 成功事件携带事务、项目、文档、提交后 RevisionSet 和事务内 sequence；
   - Phase 4 不实现 EventBus，也不在 DocumentStore 锁内调用回调。
5. AppKernel 集成
   - AppKernel 拥有 DocumentStore 和 TransactionManager；
   - 存在活动事务时 shutdown 返回 `Kernel.ActiveTransactions`；
   - 调用方必须在销毁 AppKernel 前提交、回滚或释放全部事务；AppKernel 生命周期不支持并发驱动。

## 自动化证据

环境：Windows、Visual Studio 2022、x64、MSVC 19.44.35216、Windows SDK 10.0.26100.0。

```powershell
cmake --build --preset vs2022-debug --parallel 12
ctest --preset vs2022-debug --output-on-failure

cmake --build --preset vs2022-release --parallel 12
ctest --preset vs2022-release --output-on-failure

ctest --preset vs2022-debug --repeat until-fail:20 --output-on-failure

cmake -S . -B build/production-only -G "Visual Studio 17 2022" -A x64 `
  -DLCNC_BUILD_TESTING=OFF -DLCNC_WARNINGS_AS_ERRORS=ON
cmake --build build/production-only --config Release --parallel 12
```

结果：

- Debug：54/54 CTest 通过；
- Release：54/54 CTest 通过；
- Debug repeat：54 项测试连续 20 轮通过，共 1080 次测试执行；
- Production-only Release：Foundation、Kernel、State、Runtime、Infrastructure 及生产依赖构建通过；未生成 Catch2 源码或测试目标；
- `git diff --check` 通过；
- 架构门禁通过，并覆盖 Kernel 公共头文件和生产源码边界。

## 自动化覆盖重点

- 原子对象、Revision、change set 和事件提交；
- 事务 rollback、析构放弃、中毒与 cause 链；
- 显式前置条件、重复 TransactionId、文档缺失和完整修订冲突；
- 同文档并发单胜者、跨文档 ProjectRevision 冲突与重试；
- stable ObjectId 禁止事务内重生，空净变更不能推进 Revision 或释放事件；
- Revision 溢出不改变原集合；
- AppKernel 活动事务关闭保护；
- CommittedDomainEvent 不能公开默认构造或聚合构造。

## 边界与后续

本交付只证明 Phase 4 内存状态与应用事务契约，不证明 SQLite 持久化原子性、Undo/Redo、崩溃恢复、EventBus 投递、跨进程一致性，也不证明任何 CAD/CAM 行为、GUI 交互、控制器 SDK 或物理设备能力。

下一阶段严格按蓝图进入 Phase 5：CommandRuntime、QueryRuntime、EventBus。Phase 5 必须消费成功 TransactionCommit 中的 CommittedDomainEvent，不能重新构造或提前发布事件；Phase 8 才允许将 change set、Snapshot、Journal 与 SQLite Persistence 编排接入应用事务。
