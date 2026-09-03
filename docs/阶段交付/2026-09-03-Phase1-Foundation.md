# Phase 1 Foundation 交付记录

- 日期：2026-09-03
- 阶段：Phase 1
- 状态：已验收
- 范围：Application Kernel Foundation

## 本次交付

1. 建立 CMake 3.25+、C++20 与 Visual Studio 2022 x64 构建基线。
2. 实现 `StrongId<Tag>`，提供编译期类型隔离、受检创建与稳定 Hash。
3. 实现不依赖 JSON/Qt/第三方库的递归 `Value` 模型。
4. 实现统一 `ErrorCode`、`ErrorCategory`、`Severity`、`Error` 原因链和 `Result<T>`/`Result<void>`。
5. 实现 `Version`、`Schema`、`ISchemaValidator` 与 `IValueSerializer` 公共契约；Schema 的约束仍使用 Kernel `Value`。
6. 集中声明 Development Dependency Catch2，并固定到不可变提交。
7. 增加公共 API 边界测试，阻止 spdlog、SQLite、toml11、jsoncons、线程池、Qt Widgets 和 OCCT 类型进入 Kernel 公共头文件。
8. 建立中文架构规则、开发路线图和第三方依赖清单。

## 验证证据

环境：MSVC 19.44.35216、Windows SDK 10.0.26100.0、CMake 3.29.3。

```powershell
cmake --preset vs2022-debug
cmake --build --preset vs2022-debug --parallel 16
ctest --preset vs2022-debug

cmake --build --preset vs2022-release --parallel 16
ctest --preset vs2022-release
```

结果：

- Debug：11/11 通过。
- Release：11/11 通过。
- 自有库与测试目标启用 `/W4 /WX /permissive- /Zc:__cplusplus /utf-8`。
- `architecture.kernel_public_api_boundary` 通过。

## 验证中发现并关闭的问题

1. `Result<void>` 初版缺少 `<optional>`，由 MSVC 编译门禁发现并修复。
2. Catch2 中文测试名经 CTest 过滤时受 Windows 代码页影响，导致发现名称与执行过滤器不一致。机器测试标识已改为 ASCII，中文说明保留在文档中，从而保证不同系统代码页下的稳定执行。
3. 初版 Schema 构造允许非对象约束和空单位。现已改为受检工厂，失败统一返回 Kernel Error。
4. StrongId 现拒绝空值、空白和控制字符；空 `const char*` Value 明确映射为 Null，避免静默变成空字符串。

## 边界与未实现项

- 本阶段只提供 Foundation 语义，不包含 AppKernel、模块生命周期、Command、Query、Transaction、Task 或持久化实现。
- Catch2 仅为 Development Dependency，不进入正式 `lasercnc_foundation` 链接依赖。
- 自动化通过不代表任何 CAD/CAM、GUI、控制器或物理设备能力已实现或验收。
- Production Infrastructure Dependencies 尚未引入，将在 Phase 3 经 Adapter 边界接入。

## 下一阶段准入

Phase 2 可以开始，但仅允许实现：

- AppKernel 组合根；
- ServiceRegistry 的注册、解析与冻结；
- ModuleRuntime 的描述、依赖 DAG、确定性生命周期与失败回滚。

不得提前实现领域模块、GUI、外部 RPC 或硬件能力。
