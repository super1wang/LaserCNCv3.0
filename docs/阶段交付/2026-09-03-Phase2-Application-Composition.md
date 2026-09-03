# Phase 2 Application Composition 交付记录

- 日期：2026-09-03
- 阶段：Phase 2
- 状态：已验收
- 范围：AppKernel、ServiceRegistry、ModuleRuntime

## 本次交付

1. 新增 `LaserCNC::Kernel` 静态库，并保持对上层模块和第三方基础设施零依赖。
2. AppKernel 负责模块加入、启动、服务冻结、状态管理与关闭。
3. ServiceRegistry 使用 ServiceId 与接口类型双重校验，支持线程安全解析、快照和不可逆 Freeze。
4. ModuleDescriptor 覆盖名称、版本、依赖、Required/Provided Services 以及未来 Command/Query/Task/Event/Capability 稳定名称。
5. ModuleRuntime 自研依赖 DAG、语义版本兼容检查和稳定拓扑排序。
6. 模块按 Register、Initialize、Start 三阶段启动，全部成功后统一 Ready；正常关闭按依赖逆序执行。
7. 生命周期返回错误或抛出异常时统一转换为 Kernel Error，并按逆序回滚模块和服务。
8. 回滚中的单个停止失败不会阻断其余清理；返回错误同时保留原始启动失败与回滚失败信息。

## 验证证据

环境沿用 Phase 1：MSVC 19.44.35216、Windows SDK 10.0.26100.0、CMake 3.29.3。

```powershell
cmake --build --preset vs2022-debug --parallel 16
ctest --preset vs2022-debug

cmake --build --preset vs2022-release --parallel 16
ctest --preset vs2022-release

ctest --preset vs2022-debug --repeat until-fail:10

cmake -S . -B build/vs2022-production -G "Visual Studio 17 2022" -A x64 `
  -DLCNC_BUILD_TESTING=OFF -DLCNC_WARNINGS_AS_ERRORS=ON
cmake --build build/vs2022-production --config Release --parallel 16
```

结果：

- Debug：22/22 通过。
- Release：22/22 通过。
- Debug 全集连续 10 轮通过，共 220 次测试执行无失败。
- 关闭测试依赖的 Production-only Release 配置与构建通过，未下载或链接 Catch2。
- 公共 API 边界检查覆盖新增 Kernel 头文件并通过。
- 自有目标继续使用 `/W4 /WX /permissive- /Zc:__cplusplus /utf-8`。

## 覆盖的关键场景

- 类型安全服务注册、解析、缺失、重复、空实例与类型不匹配；
- Registry Freeze 后拒绝组合修改；
- 与加入顺序无关的确定性依赖启动及逆序停止；
- Missing Dependency、Version Conflict、Circular Dependency；
- Required/Provided Service 声明与实际注册不一致；
- 多个模块声明同一服务提供者时在回调前拒绝启动；
- Start 失败后的服务和模块完整回滚；
- 生命周期异常的边界转换；
- 回滚停止失败时继续清理并报告组合错误；
- Ready 后拒绝追加模块。

## 边界与未实现项

- 没有新增第三方依赖；正式 Kernel 仍只依赖 C++20 标准库和 Foundation。
- 当前是静态/进程内模块运行时，不包含动态插件加载。
- Command、Query、Task、Event、Capability 目前只有描述符级稳定 ID，执行语义将在对应阶段实现。
- 自动化证据仅证明当前内核组件行为，不代表任何上层功能、GUI 或物理设备通过验收。

## 下一阶段准入

Phase 3 仅允许在 `infrastructure/` 下接入固定版本的：

- spdlog logging adapter；
- jsoncons serializer/schema adapter；
- toml11 config adapter；
- SQLite persistence adapter；
- BS::thread_pool executor adapter。

每个 Adapter 必须面向 Kernel 接口、转换第三方错误、避免第三方类型进入公共 API，并补充依赖版本、许可证和可替换性测试。
