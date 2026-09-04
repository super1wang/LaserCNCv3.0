# K10F-F5C 执行边界与状态终审

## 状态

实现已完成，三配置单轮全集与纯生产门禁通过，连续认证进行中，未验收。F5B 固定版本 `68580d7` 已完成三配置全集、纯生产和 21 组性能重测。本节点补充以下三个内核收口项，不将局部测试全绿解释为整体 Frozen。

## 必须完成的修正

1. AppKernel 仍返回可变 Scheduler，Host 可直接 start/configureExecutor/shutdown/requestCancel。移除可变 getter，只保留观察；合法执行器配置继续用 configureTaskExecutor，任务取消走 ExecutionGateway，整体停止走 AppKernel.shutdown。
2. CommandRegistry 的四类 handler 注册和 QueryRegistry 注册均未验证 ContractStatus；只定义了 Active/Deprecated，却允许非法枚举安装并通过响应返回。补充 fail-closed 检查、五类注册的合法/非法对照、忽略注册错误仍拒绝启动的模块测试。
3. ScopeQueryHandler/ScopeCommandHandler 已返回 hasDocument，但原四 scope 用例主要断言 hasValue/calls。补充返回内容、Query revisions、只读 Command 无 commit/taskId 以及文档状态未变的直接证据。

已确认 ResourceManager 的 tryAcquire/release 是私有方法，仅 Scheduler/EffectExecutor 可调用；不为此增加或改造资源控制面。独立组件仍可自行配置、启动和测试，约束针对 Host 通过 AppKernel 获得的运行对象，不宣称同进程 C++ 沙箱。

## 验证要求

先证明未知 ContractStatus 的回归测试能在修正前失败，再完成实现、三配置全集、纯生产及架构正负例。最终重复矩阵以最后代码版本为准：完整 Debug 连续 3 次，F1 故障矩阵各 20 次、29 个独立进程恢复各 20 次；F3 的 12 个压力用例须逐项确认包含在完整重复结果中。测试数量在新增用例后重新清点。

生产代码改变后重新采集并归档最终基线，不沿用 F5B 二进制摘要。范围仍仅内核；独立 Project 生命周期范围尚待用户确认，不能通过修改验收定义省略。

## 实现与专项证据

- AppKernel 仅返回 const Scheduler，普通 Host 也不能调用 start、shutdown、configureExecutor 或 requestCancel。编译期断言覆盖普通/const Host 的四种不可达方法，以独立可变 Scheduler 为正对照；架构扫描同时检验健康头文件和恢复可变 History、Persistence、Scheduler getter 的三个负例。
- 新增 validContractStatus，四类 Command 注册及 Query 注册只接收 Active/Deprecated，其他枚举值返回 Command.InvalidStatus 或 Query.InvalidStatus。注册矩阵覆盖 5 类 handler × 5 个状态值（0、1、2、127、255）；模块用例验证即使回调忽略错误，启动仍失败、initialize 未调用且 Registry 为空。
- 四 scope 的 Query/同步只读 Command 用例直接检查返回 hasDocument 与 scope 一致；Query revisions 的存在性一致，只读 Command 无 commit/taskId。执行前后文档对象和 revisions 均保持不变。

未知状态测试先在旧实现运行并确认失败。首次夹具误保留了同步只读命令的幂等标志，产生额外注册错误，其日志 `build/k10f-f5c-status-red.log` 保留但不作为纯粹缺陷证明。修正夹具后、生产修正前，`build/k10f-f5c-status-red-corrected.log` 记录 50 条断言中 20 通过、30 失败（CTest 退出 8）：10 个合法场景通过，15 个非法注册场景分别在返回值和注册数量上失败。

修正后专项 5/5 通过（0.58 秒），见 `build/k10f-f5c-focused-tests.log`。该专项之后补充了 initialize 未调用的显式断言；后续完整回归包含该最终断言，专项不能冒充最终全集。

完整 Debug 已通过 264/264（307.16 秒），日志 `build/k10f-f5c-debug-tests.log`；构建日志 `build/k10f-f5c-debug-build.log`。包含最后补充的 initialize 断言及三个 Host 负例，据此形成本地实现检查点。Release、ASan、纯生产、连续重复与新基线尚待完成。

## 固定版本三配置检查点

实现提交 `0cbd348`。以下为随后顺序完成的实际结果；后续工作区只更新中文文档，没有改变被测实现。

| 门禁 | 实际结果 | 日志 |
| --- | --- | --- |
| Debug | 264/264，307.16 秒 | `build/k10f-f5c-debug-tests.log` |
| Release | 264/264，313.53 秒 | `build/k10f-f5c-release-tests.log` |
| ASan RelWithDebInfo | 267/267，308.52 秒 | `build/k10f-f5c-asan-tests.log` |
| 纯生产 Release | 31 工程；0 测试/contract/Benchmark/ASan 工程、0 插桩配置、0 CTest 文件 | `build/k10f-f5c-production.log` |
| 架构正负例 | 69 个公共头、133 个生产源通过；三全集包含 Host/OCCT 负例 | 同上及各全集日志 |

三个全集分别核对实际 Passed 行及唯一编号，均与上述数量一致。日志 SHA-256：

- Debug：`CD4A4CE9A6D00D76A7AE357111AD96B2C905A7F81F9CE265F501BB506D42250F`。
- Release：`E80EA592C8EDE3444206A4CB270031C7D131F6A1FE4364401B7EE303D003B690`。
- ASan：`A17582E8BB73EC91BBC7B798AA3CF8DE91FB6913B41C398F32A276F6CE19627F`。

最终连续认证已开始，清单固定为全集 264、故障矩阵 9、独立进程恢复 29、压力用例 12。只有实际完成并逐项核对次数后才记录通过；三配置单轮不替代这些重复门禁。正式 Release 基线亦待重新采集归档。
