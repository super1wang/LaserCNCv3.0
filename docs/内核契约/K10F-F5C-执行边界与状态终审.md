# K10F-F5C 执行边界与状态终审

## 状态

进行中，未验收。F5B 固定版本 `68580d7` 已完成三配置全集、纯生产和 21 组性能重测。进一步源码审计确认以下三个内核收口项；实现前不将现有测试全绿解释为整体 Frozen。

## 必须完成的修正

1. AppKernel 仍返回可变 Scheduler，Host 可直接 start/configureExecutor/shutdown/requestCancel。移除可变 getter，只保留观察；合法执行器配置继续用 configureTaskExecutor，任务取消走 ExecutionGateway，整体停止走 AppKernel.shutdown。
2. CommandRegistry 的四类 handler 注册和 QueryRegistry 注册均未验证 ContractStatus；只定义了 Active/Deprecated，却允许非法枚举安装并通过响应返回。补充 fail-closed 检查、五类注册的合法/非法对照、忽略注册错误仍拒绝启动的模块测试。
3. ScopeQueryHandler/ScopeCommandHandler 已返回 hasDocument，但原四 scope 用例主要断言 hasValue/calls。补充返回内容、Query revisions、只读 Command 无 commit/taskId 以及文档状态未变的直接证据。

已确认 ResourceManager 的 tryAcquire/release 是私有方法，仅 Scheduler/EffectExecutor 可调用；不为此增加或改造资源控制面。独立组件仍可自行配置、启动和测试，约束针对 Host 通过 AppKernel 获得的运行对象，不宣称同进程 C++ 沙箱。

## 验证要求

先证明未知 ContractStatus 的回归测试能在修正前失败，再完成实现、三配置全集、纯生产及架构正负例。最终重复矩阵以最后代码版本为准：完整 Debug 连续 3 次，F1 故障矩阵各 20 次、29 个独立进程恢复各 20 次；F3 的 12 个压力用例须逐项确认包含在完整重复结果中。测试数量在新增用例后重新清点。

生产代码改变后重新采集并归档最终基线，不沿用 F5B 二进制摘要。范围仍仅内核；独立 Project 生命周期范围尚待用户确认，不能通过修改验收定义省略。
