# Phase 2 应用组合与模块生命周期契约

本文定义 `AppKernel`、`ServiceRegistry` 和 `ModuleRuntime` 的当前稳定语义。它们只负责应用组合，不承载 CAD、CAM、Machine 等领域行为。

## AppKernel

`AppKernel` 是应用唯一组合根，状态为：

```text
Configuring → Starting → Ready → Stopping → Stopped
                    ↘ Failed ↙
```

- 只有 `Configuring` 状态可以加入模块。
- `bootstrap()` 只能成功执行一次。
- 所有模块 Ready 后才冻结 ServiceRegistry，并将 AppKernel 切换为 Ready。
- 显式 `shutdown()` 按依赖逆序停止模块；析构仅提供 Ready 状态下的尽力清理，宿主仍应显式关闭并处理错误。

## ServiceRegistry

服务以稳定 `ServiceId` 和 C++ 接口类型双重定位：

- 重复 ID、空服务、缺失服务和类型不匹配均返回统一 Kernel Error。
- Registry Freeze 后拒绝任何新注册，但继续允许并发读取和快照。
- 模块注册的实际服务集合必须与 `ModuleDescriptor::providedServices` 完全一致。
- ServiceRegistry 只用于组合和基础设施定位；业务对象优先使用构造注入，不允许在业务逻辑中散布 Service Locator 调用。

## ModuleDescriptor

描述符包含：

- 模块稳定 ID、显示名与语义版本；
- 模块依赖及最低兼容版本；
- Required/Provided Services；
- Commands、Queries、Tasks、Events、Capabilities 的稳定名称声明。

当前依赖兼容规则为：提供模块的主版本必须与最低版本主版本一致，且完整版本不得低于最低版本。例如 `1.4.0` 满足 `>=1.2.0`，`2.0.0` 不满足 `1.x` 依赖。

## 依赖图与确定性

ModuleRuntime 在任何回调执行前一次性验证：

1. 模块 ID 唯一；
2. 依赖存在且不重复；
3. 依赖版本兼容；
4. 依赖图无环；
5. Required Service 声明不重复；
6. Provided Service 在模块间只有一个提供者。

拓扑排序使用 ModuleId 字典序作为同层节点的稳定决胜规则，因此模块加入顺序不会改变合法组合的启动顺序。

## 生命周期

单个模块遵循：

```text
Discovered
  → Registered
  → Initialized
  → Started
  → Ready
  → Stopping
  → Stopped
```

失败模块进入 `Failed`。启动流程分三轮执行，确保所有服务先注册，再初始化，最后启动：

1. 按拓扑序调用 `registerComponents(ModuleRegistrar&)`，并核验全部模块贡献声明；这是 K10D D1 对原 `registerServices()` 契约的收紧。
2. 检查 Required Services 后按拓扑序调用 `initialize()`。
3. 按拓扑序调用 `start()`；全部成功后统一进入 Ready。

生命周期回调抛出的标准或未知异常都必须在 ModuleRuntime 边界转换为 Kernel Error，不得穿透 AppKernel。

## 回滚与关闭

- 任一 Register、Initialize 或 Start 失败都会触发依赖逆序回滚。
- `stop()` 必须能处理仅完成注册或初始化的部分状态。
- 回滚会移除本次模块注册的服务，恢复到 bootstrap 前的 ServiceRegistry 内容。
- 某个 `stop()` 失败时仍继续清理其余模块和服务，并返回 `Kernel.ModuleRollbackFailed`，原始启动错误保留在 cause 中，首个回滚错误记录在 details 中。
- 正常关闭同样会尝试停止全部模块，不因单个停止错误提前退出。

## 线程模型

- 模块发现、加入、bootstrap 和 shutdown 由宿主组合线程串行调用。
- ServiceRegistry 内部使用读写锁，Freeze 后可供运行期并发解析。
- ModuleRuntime 的生命周期快照当前面向组合线程；面向运行期的并发诊断快照将在 Phase 7 统一建设，不能提前把观测逻辑耦合进业务控制流。

## 本阶段非目标

- 不动态加载 DLL，不引入 CTK、OSGi、IoC 或依赖注入框架。
- 不实现 Command/Query/Event 的注册与执行；描述符仅预留稳定名称。
- 不实现配置、数据库、线程池或日志后端。
- 不实现任何 CAD、CAM、Machine、GUI 或硬件能力。
