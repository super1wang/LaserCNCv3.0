# ST1C6b10 Diagnostics 并发与 latest 顺序

## 语义选择

`DiagnosticsService::latest` 定义为每个 DiagnosticId **最后完成本地发布** 的报告：check 已返回或已被转换为 Unhealthy，报告已写入本地 latest，且本次 exporter 快照已经复制。它不是最后开始的调用、最后返回给调用方的调用，也不等待 exporter 调用完成。`observedAt` 由服务在 check 返回后赋值；系统时钟回拨不在本节点改写为单调时钟。

同一注册项从进入 `IDiagnosticCheck::run` 到上述本地发布完成必须串行。端口没有要求第三方 check 自行线程安全，Kernel 不能让同一实例状态被两个同 ID 调用同时修改，也不能让较早调用在较晚调用发布后覆盖 latest。不同 DiagnosticId 使用独立包装器，仍可并行执行；exporter 继续在服务锁和注册项执行锁之外调用。

## 递归与错误

如果 check 在同一线程递归运行自己的 DiagnosticId，服务不等待自身，而将内层调用转换为 Unhealthy 报告，`details.errorCode` 为 `Diagnostics.CheckReentered`。这遵循既有“检查错误转换为报告”的边界，外层 `run` 仍返回成功的 DiagnosticReport；该错误不代表 AppKernel 或业务执行失败。check 读取 `latest` 仍安全。

并发等待当前没有取消或 deadline 参数，也不承诺公平调度；这些同步预算归 C6d。注册包装器和报告复制的资源失败、check 跨不同 ID 复用同一底层对象、全局检查数量及 details 预算仍需分别审计。

## 验证

受控双线程测试先让同 ID 第一次 check 阻塞，再启动第二次：旧实现可同时进入，最大并发为 2，第二次先发布后又被第一次覆盖为 `run.1`。修复后第二次直到第一次 latest 发布完成才进入，最大并发为 1，最终 latest 为 `run.2`，两份 `observedAt` 按本地发布顺序排列。测试另证明两个不同 ID 可同时进入，以及同 ID 递归在 watchdog 限期内返回 `Diagnostics.CheckReentered` 而不死锁。

本节点只覆盖本地 Diagnostics 注册项并发和 latest 语义，不增加远程诊断、自动恢复、产品 Host、GUI、设备或任何上层模块。
