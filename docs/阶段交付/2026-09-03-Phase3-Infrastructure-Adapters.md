# 2026-09-03 Phase 3 Infrastructure Adapters 交付

## 结论

Phase 3 已验收。五项 Production Infrastructure Adapter 均已落地，Kernel 公共 API 不含第三方类型，第三方失败均在边界转换为统一 Error。本阶段严格停留在内核端口与基础设施实现，没有扩展 CAD、CAM、GUI、CLI、Workflow、TaskRuntime 或 PersistenceService。

## 交付内容

1. 依赖治理
   - spdlog、jsoncons、toml11、BS::thread_pool 固定到不可变 Git commit；
   - SQLite 3.53.4 固定官方 amalgamation URL 与 SHA3-256；
   - Catch2 单独归类为 Development Dependency；
   - 所有 Git 依赖关闭无关 submodule 递归获取。
2. 日志
   - `SpdlogLogService` 提供控制台、轮转人工文件、轮转 JSONL；
   - JSONL 保存完整 Kernel 日志字段和结构化数据；
   - 初始化、序列化、写入、刷新失败转换为 Kernel Error。
3. 序列化与配置
   - `JsonconsAdapter` 完成 Value JSON 往返和 Kernel Schema 校验；
   - `TomlConfigAdapter` 完成对象配置解析/序列化并拒绝无损表达不了的类型。
4. 持久化
   - `SqlitePersistenceBackend` 完成连接 RAII、参数化单语句、标量行映射和真实文件持久化；
   - 提供底层 begin/commit/rollback，但不冒充 Application Transaction；
   - 复合参数、BLOB、重复列名和多语句均 fail-closed。
5. 执行后端
   - `BsThreadPoolExecutor` 完成提交、完成回调、异常转换、等待空闲和有界排空关闭；
   - 并发 shutdown 串行化，关闭后拒绝新工作，worker 自等待 fail-closed；
   - TaskId、取消、优先级、资源和持久化没有下沉给第三方线程池。
6. 架构门禁
   - 原公共 API 三方类型扫描保留；
   - 新增生产源码目录隔离扫描，防止某个三方库被其他 Kernel 目录直接引用。

## 自动化证据

环境：Windows、Visual Studio 2022、x64、MSVC 19.44.35216、Windows SDK 10.0.26100.0。

```powershell
cmake --preset vs2022-debug
cmake --build --preset vs2022-debug --parallel 12
ctest --preset vs2022-debug

cmake --build --preset vs2022-release --parallel 12
ctest --preset vs2022-release

ctest --preset vs2022-debug --repeat until-fail:20

cmake -S . -B build/production-only -G "Visual Studio 17 2022" -A x64 `
  -DLCNC_BUILD_TESTING=OFF -DLCNC_WARNINGS_AS_ERRORS=ON
cmake --build build/production-only --config Release --parallel 12
```

结果：

- Debug：40/40 CTest 通过；
- Release：40/40 CTest 通过；
- Debug repeat：40 项测试连续 20 轮通过，共 800 次测试执行；
- Production-only Release：Foundation、Kernel、Infrastructure、spdlog、SQLite 构建通过；`build/production-only/_deps/catch2-src` 不存在，未生成测试目标；
- `git diff --check` 通过；
- 架构门禁检查公共头文件和生产源文件，禁止第三方类型/API 越界。

## 边界与后续

本交付只证明 Phase 3 基础设施契约及其自动化范围，不证明任何 CAD/CAM 行为、GUI 交互、控制器 SDK 或物理设备能力。

下一阶段仅按蓝图进入 Phase 4：Document、ObjectRegistry、Revision、Transaction。SQLite 仍只是持久化 backend；在 Phase 4 明确业务一致性、Revision 和提交后事件规则之前，不新增上层 Repository 或领域数据表。
