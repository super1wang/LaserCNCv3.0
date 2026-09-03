# LaserCNC v3.0

LaserCNC v3.0 当前处于 Application Kernel 建设阶段。仓库按
Command-First、Automation-First、Headless-First 和 Infrastructure-Adapter
原则推进；在内核阶段结束前，不引入 CAD、CAM、Machine 或 Qt GUI 实现。

## 当前阶段

- Phase 1：Foundation
- 范围：`StrongId`、`Value`、`Result`、`Error`、`Schema`
- 标准：C++20
- 测试：Catch2 + CTest

详细状态见 [内核开发路线图](docs/内核开发路线图.md)，强制边界见
[内核架构规则](docs/内核架构规则.md)。原始设计依据保存在
[最终架构设计方案](LaserCNC%20Application%20Kernel%20最终架构设计方案.md)。

## Windows 构建

```powershell
cmake --preset vs2022-debug
cmake --build --preset vs2022-debug --parallel
ctest --preset vs2022-debug
```

构建产物位于 `build/vs2022`，该目录不进入版本控制。
