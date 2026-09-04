# K10F-F5C 执行边界与状态终审

## 状态

F5C 工程加固节点已验收：三配置全集、纯生产、连续认证与新基线全部通过。作为 F5A/B/C 累积工程加固大节点同步远端，但不签发 Kernel 1.0 Frozen；独立 Project 生命周期仍待范围确认。F5B 固定版本 `68580d7` 的历史门禁继续保留。

## 修正前的审计发现

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

连续认证启动时，清单固定为全集 264、故障矩阵 9、独立进程恢复 29、压力用例 12。以下记录其完成结果，未用三配置单轮替代重复门禁。

## 连续认证结果

| 门禁 | 实际执行 | 耗时 | 日志 |
| --- | --- | --- | --- |
| 完整 Debug | 264 个用例各 3 次，共 792 次，通过 | 896.37 秒 | `build/k10f-f5c-full-repeat3.log` |
| F1 故障矩阵 | 9 个用例各 20 次，共 180 次，通过 | 38.13 秒 | `build/k10f-f5c-fault-repeat20.log` |
| F2 独立进程恢复 | 29 个用例各 20 次，共 580 次，通过 | 645.80 秒 | `build/k10f-f5c-recovery-repeat20.log` |
| F3 并发/生命周期 | 全集重复内的 12 个用例各 3 次，共 36 次，逐项通过 | 已包含在全集耗时 | 同完整 Debug 日志 |

核验按每条实际 Passed 记录的用例编号分组，不只读取 CTest 最后摘要；同时核对唯一用例数和每组重复次数。F2 保留 27 个三进程场景与 2 个早期二进程场景，对应 1,700 个子进程。F1 新增的独立连接隔离用例保留，因此从历史 8 个矩阵扩展为 9 个，不缩减原 100 个故障场景。

日志 SHA-256：全集 `43A3BA094702F0CBB1D0565DF3DC3EF851DEA7C4DD81B76F82D82BB0F09C19B2`；故障 `1A4B0C50D6B22C88AC689C680511A713A6F50A524E7FD9F1F16782338D97861A`；恢复 `2D9EB3F13BA510A9702164063AB7296EA04D4EBE7F8778C06754C571E5C0C29D`。

本轮重复范围内未出现 flaky、deadlock 或 partial state。该结论限于已记录的版本、参与者、轮次及同步交错，不是所有线程调度、物理掉电或设备安全的证明。开发期失败日志仍保留，未覆盖或删除。

## 最终版本性能与内存基线

09:12:50–09:14:26 在 `f32de3a34b2deeb04a7b36e618c4e93a8f3f9c36` 干净工作区串行采集；该提交相对实现 `0cbd348564c5387e54bdf743b2928685a671bf13` 仅修改文档。采样不与构建、压力测试或 ASan 并行。

21 份原始报告已逐字节归档，逐份核对 SHA-256；共 81 个操作项目、405 个原始样本、60 轮生命周期。见 [归档索引](evidence/k10f-f5c/index.json)，本机原索引为 `build/kernel-baselines/run-20260904-091249-e186632556e24c70ac29a6cbf7316d7b/index.json`。归档索引摘要 `BF3DE0B888AE063F20BF6297A5BCDEF761B12C59220DC6F2FC225ED09414C959`。

Release 程序 SHA-256 为 `5C5B3DCBBFF927A1D8FF46F92DDB9855DE15781A5C1CE282B1A91A373214D4A2`；Benchmark 源码摘要为 `A6261B4180AEED6B2CE57E36469C16FD7F19EB1695CC56BDDD43E5B0D94488F2`。不能沿用 F4 或 F5B 的程序摘要。

数据口径保持 1k/10k/100k 对象、128 字节字符串、142 字节序列化 Value、每对象 0 资产，预热 1、样本 5、每组生命周期 10 轮。机器、编译器、真实 SQLite 设置及逐次内存值见索引和原报告。F5B 引入的独立 Snapshot 连接初始化仍计入 seed/lifecycle 启动时间。

100k 代表性中位数（毫秒）：

| 操作 | 内存 | SQLite |
| --- | ---: | ---: |
| Document snapshot | 39.75 | 35.85 |
| Transaction begin | 72.62 | 73.65 |
| Transaction commit | 175.78 | 250.94 |
| DocumentWrite 网关 | 280.77 | 369.27 |
| Undo 网关 | 279.77 | 371.99 |
| Redo 网关 | 268.62 | 385.85 |

SQLite Journal append / recovery material / Kernel bootstrap recovery 分别为 73.69 / 480.23 / 670.15 ms。100k 生命周期第 0→9 轮销毁后的 private bytes：内存模式 2,351,104→4,251,648，SQLite 模式 4,902,912→8,261,632；原始各轮均保留，存在残留与分配器缓存影响，不能宣称零泄漏。进程峰值包括初始化，不能用来代表单个操作峰值。

沿用 F4 的结构性结论：保留当前快照/事务模型，明确全量复制和持久化成本；没有业务 SLA 或针对性瓶颈证据时不引入 immutable snapshot/delta 重构。单轮采样波动不能证明无性能回退，当前数字也不构成实时运动或硬件准入。

## 复现与交付边界

三配置使用 `cmake --build --preset vs2022-debug|vs2022-release|vs2022-asan --parallel 1 -- /nodeReuse:false` 顺序构建，并分别执行 `ctest --preset <配置> -j 4 --output-on-failure`。竖线表示三选一配置名，不能直接作为 shell 管道执行。

完整重复使用 `ctest --preset vs2022-debug -j 4 --repeat until-fail:3 --output-on-failure`。故障专项由测试源码 `[fault-matrix]` 标签提取 9 个精确名称，匹配 CTest 清单后各执行 20 次；恢复专项正则为 `^integration.kernel_(crash_|(persistence|workflow)_process_recovery$)`，匹配 29 项后各执行 20 次。正式基线运行 `tests/benchmark/RunKernelBaseline.ps1` 默认参数。

本节点只涉及内核所有权、执行契约、验证基础设施和中文文档。未增加领域模块或第三方依赖。完整工程门禁不覆盖尚未实现的独立 Project 生命周期；该项不能通过改写验收定义省略，整体冻结状态见 [逐项审计清单](Kernel-1.0-冻结审计清单.md)。
