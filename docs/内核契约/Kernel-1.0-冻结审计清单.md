# Kernel 1.0 冻结逐项审计清单

## 当前工作状态：ST1 补齐中

2026-09-04 用户已确认按交叉审计补齐计划。当前独立待闭合项见 [ST1C 补充执行计划](ST1C-补充审计与剩余执行计划.md)：C1 修订恢复/写入检查、C2 整体准入和析构、C3 存储键、C4 attach、C5 独占与耐久配置、C6–C8 冻结契约/容量/门禁。历史 31/32 不表示当前仅剩一项；对应条目按新增回归重开审计，不否定原用例的真实成功记录。

C1a 已修复全部文档 Detached/Removed 后重启丢失 ProjectRevision 的缺陷；旧代码先复现失败，修复后 Debug 286/286、专项 16/16、新增 2 项各 3 次、纯生产及架构检查通过，见 [C1a 交付](../阶段交付/2026-09-04-ST1C1a-项目修订独立恢复.md)。该本地检查点不关闭 C1b/C5 写入端与独占权，也不替代 ST1D 最终签核。

复核原规划总览、K10B 标题和 State 清单后，独立 Project 生命周期被确认为原目标内的必需能力，不再作为额外范围决策阻塞。ST1A 的持久组件以及 ST1B 的 ProjectRuntime、文档联动和关闭协调均已通过本地检查点。Project-only 执行准入、完整恢复与最终认证仍未签核，不能宣布 Frozen；详见 [ST1 收口契约](ST1-独立项目生命周期收口.md)。

ST1 将影响状态、执行、持久化和恢复；以下 31 项通过记录属于 F5C 固定版本，不自动延伸到新增代码。所有受影响项须在 ST1D 完成三配置、故障/恢复/压力与新基线后重新签核，当前仍不得宣布 Frozen。

C5a/b 已形成会话端口、实际文件独占和初始化强制准入的本地检查点，见 [C5b 交付](../阶段交付/2026-09-04-ST1C5b-初始化独占与活动状态保护.md)：Debug 299/299、专项 56/56、新增 5 项各 10 次及纯生产/架构通过。第二 Kernel 不改写活动 claim 的原始回归红转绿，跨进程拒绝与接管、策略漂移和回滚隔离后保锁均已验证。C5 的最终签核仍与 C2c drain/析构、支持矩阵和 ST1D 联动，不以本地检查点代替整体冻结。

C1b 已在独占连接写事务内核验持久修订、合法变化和首笔 Catalog/Snapshot 归属，见 [C1b 交付](../阶段交付/2026-09-04-ST1C1b-Journal写入修订一致性.md)：Debug 308/308、专项 14/14、新增 9 项与扩展故障矩阵各 10 次通过。实际事件暂存、零交付和重启成功交付均有断言，失败不发布对象、修订、History 或成功回执。下一步 C2；正式容量、三配置认证和整体 Frozen 仍须 ST1D 重新签核。

C2a 已补齐整体准入和停止协调，见 [C2a 交付](../阶段交付/2026-09-04-ST1C2a-整体准入与停止线性化.md)：Debug 314/314、专项 7/7、7 项各 10 次通过。原始生命周期/观察发布中途停止已红转绿，首轮遗漏失败 trace/metrics 的回退也已修复且保留日志。下一步 C2b/C2c；Project-only 活动、目录失效、非协作任务 drain/析构及 ST1D 仍未签核。

C2b1 已完成普通 Project-only 执行、无文档 Task 长期桥接及终态发布中的关闭保护，见 [C2b1 交付](../阶段交付/2026-09-04-ST1C2b1-项目活动与任务关闭桥接.md)：Debug 320/320、专项 13/13、13 项各 10 次及纯生产/架构通过。原始 3 项红灯已转绿，接受写入失败/异常、执行器异常和项目隔离均已验证。下一步 C2b2/3；生命周期控制分类、Workflow/Script 归属、legacy 恢复、目录失效、C2c 和 ST1D 仍未签核，不能以此宣布 Frozen。

C2b2a 已修复 Workflow/Script 错误项目归属接纳、终态发布期间提前关闭，以及取消检查点回调中关闭文档先写 Closing 引发的重入问题。见 [C2b2a 交付](../阶段交付/2026-09-04-ST1C2b2a-编排活动与关闭预检.md)：Debug 324/324、20 项各 10 次和纯生产/架构通过；拒绝关闭无 SQL、未保存终态阻塞及重试成功均有断言。生命周期命令分类、终态/legacy 恢复仍在 C2b2 待办，C2b3/C2c/ST1D 仍未签核。

## F5C 固定版本结论（历史检查点）

尚未冻结。本表逐项承接《Kernel 1.0 最终收口设计规划》第 5 节。F5C 最终实现 `0cbd348` 已完成 Debug/Release 各 264/264、ASan 267/267、纯生产与架构门禁，全集各 3 次共 792 次、故障矩阵各 20 次共 180 次、独立进程恢复各 20 次共 580 次；21 份新基线报告已归档。逐项证据已回链至本轮受测代码和实际断言，除 ST1 的独立 Project 生命周期外，其余 31 项在本文列明的软件契约和验证范围内通过。ST1 未闭合，因此不得签发整体 Frozen。

## F5C 时发现的范围差异（已按原目标决定补齐）

F5C 时，清单要求 Project/Document 生命周期，但实现仅覆盖 DocumentRuntime 和 Project ownership。当时缺少独立 ProjectRuntime；ST1B 已补齐基础接口，剩余问题按上面的当前计划处理。

当时曾提出范围确认；之后已按原目标决定实现完整项目内核生命周期，不再等待该选择。这里保留决策来源，不作为当前阻塞原因，也不能通过改写验收定义绕过新增正确性问题。

## 第 5 节逐项证据索引

以下表格列出 F5C 逐项实现证据及签核边界；通过不等于任意输入、线程交错、同进程恶意代码或物理硬件的数学保证。F5C 的 EX2 Project scope 仅按 K10A 的上下文形状及文档所属关系核验；ST1B 虽已实现 ProjectRuntime，仍不能据此推导所有 Project-only 入口均已覆盖其存在性和生命周期准入。

补充发现及修复：Host 可变 History/Persistence 的底层状态写入口已在 F5B 封闭；Scheduler 只读入口、未知 ContractStatus 拒绝和 scope 直接断言在 F5C 补齐。F5C 经当时三配置与连续矩阵验证后签核相关项目；当时保留的 Project 范围问题现已转入 ST1 实施，不再等待确认。历史证据见 [F5B 审计](K10F-F5B-Host状态写入口审计.md) 与 [F5C 终审](K10F-F5C-执行边界与状态终审.md)。

| 编号 | 规划要求 | 当前实现与证据入口 | 终审注意点 |
| --- | --- | --- | --- |
| EX1 | Command/Query 请求版本化 | command/query.hpp、对应 Registry；command_query_runtime_tests 的 compatible/deprecated/version 用例 | Exact/Compatible、拒绝与响应解析版本均需覆盖 |
| EX2 | 四类 Execution scope 完整 | execution_scope、Command/Query Runtime；四 scope 用例及异步 Global Task 用例 | 无文档 scope 不应取得文档快照 |
| EX3 | ReadOnly/DocumentWrite/ExternalEffect 语义闭环 | CommandRuntime、EffectExecutor、TaskRuntime；执行链及故障用例 | 不让只读/外部副作用走普通文档 Undo |
| EX4 | External recovery/replay policy | effect_executor、effect_persistence；F2B Safe/Idempotent/ReconcileOnly/Never | 仅显式允许重试，不自动调用 handler |
| ST1 | Project/Document 运行期 create/open/close | 历史 DocumentRuntime；ST1B 新增 ProjectRuntime 与关闭协调 | **仍未整体闭合，按 C1–C5 补充回归，不再等待范围确认** |
| ST2 | Document 生命周期与 Task/Transaction 一致 | DocumentActivityLease、CloseBlockers；F3 Query/Task/Workflow/关闭压力 | 活动准入和长期工作检查不能有空窗 |
| ST3 | Revision 冲突 fail-closed | RevisionManager、TransactionManager；版本冲突及八路竞争用例 | 唯一持久胜者，失败候选无状态残留 |
| TX1 | DocumentWrite 唯一 Transaction 链 | AppKernel 私有 transactions_、CommandRuntime、Workflow/Script 调度 | AppKernel 不公开可变存储/事务管理器 |
| TX2 | Undo/Redo 正式可用 | HistoryRuntime、内置 edit.undo/redo；history_runtime_tests | 覆盖变更形态、分支与 barrier，不仅单次返回值 |
| TX3 | Journal/History/Revision 一致 | TransactionManager + PersistenceService；F1A/F2A/F3 | F5B 封闭 Host History.restore；最终版本回归以 F5C 为准 |
| ET1 | ModuleRegistrar | module_registrar.hpp/cpp；声明/实际贡献用例 | 首次被忽略的注册错误也必须使启动失败 |
| ET2 | Registry ownership audit | ModuleRuntime、ModuleContributionSnapshot；九类模块贡献/回滚用例 | Event/Capability 是贡献审计事实；安全 Guard 是 Kernel 配置表，不冒充新增领域 Registry |
| ET3 | Registry Ready 后冻结 | AppKernel bootstrap；各 Registry frozen 与配置拒绝用例 | 包括 ObjectType 与 EffectGuard；不同 Registry 的负责者须明确 |
| ET4 | Host 无直接 Task/Transaction 旁路 | AppKernel、ExecutionGateway、Scheduler 私有 schedule；类型断言与架构扫描 | F5B 拆分持久化配置/观察；F5C 进一步禁止 Host 直接改变 Scheduler 生命周期 |
| PE1 | Idempotency 跨重启 | PersistenceService、CommandRuntime；持久幂等与 F2A | 签名绑定版本、scope 与请求内容 |
| PE2 | Snapshot/Journal crash-safe | SQLite/Snapshot 适配器、恢复链；F1/F2 独立进程 | 软件进程终止范围，不扩大为物理掉电证明 |
| PE3 | Workflow recovery | workflow_persistence、WorkflowRuntime；F2B 三进程与 checkpoint 故障 | 固定 attempt，不自动重放不安全外部副作用 |
| PE4 | History recovery | HistoryRuntime.restore；F2A Undo/Redo 中断恢复 | 游标和前后像一致，恢复后实际可用 |
| PE5 | Asset publish crash-safe | FilesystemAssetStore；F2C 发布与引用故障 | 已提交资产丢失/损坏拒绝恢复 |
| SA1 | Capability 默认拒绝 | CapabilityService；Command/Query/Effect 前置拒绝用例 | 注册 capability 不等于授予会话权限 |
| SA2 | 外部副作用不自动重放 | EffectExecutor recover、Workflow 恢复；F2B | 使用独立调用记录确认没有隐含 handler 执行 |
| SA3 | Unknown fail-closed | 枚举/持久 wire 校验；篡改状态/策略/错误材料用例；F5C 五类 handler 状态矩阵 | ContractStatus 仅 Active/Deprecated；缺失、无效、过深或不一致材料拒绝安装 |
| SA4 | 不做 Machine-specific safety 判断 | Kernel 目录/公共头、架构扫描和 K10E-E3 | 不等于物理安全认证，不实现控制器/碰撞/运动许可 |
| SA5 | 统一 Effect Guard | IEffectGuard、EffectGuardRegistry、EffectExecutor；Guard 次序与拒绝用例 | Ready 冻结，缺少声明 guard 时启动拒绝 |
| DA1 | ObjectType version/migration | ObjectTypeRegistry、版本化 ObjectRecord；迁移与恢复准入用例 | 显式迁移、完整候选/引用验证，失败无半迁移 |
| DA2 | AssetRef/Data Plane | AssetRef、IAssetStore、资产状态测试和 F2C | 大资产在文件侧，历史前像引用也验证 |
| DA3 | 第三方类型不进入 Kernel API | VerifyKernelBoundaries、OCCT 负例 | 正扫描与反例同跑，不能只看链接成功 |
| RE1 | Debug/Release 全绿 | F5C 双配置各 264/264，ASan 267/267 | 固定实现 0cbd348；旧版本结果不替代本轮 |
| RE2 | 无 flaky test | 全集 792 次、故障 180 次、恢复 580 次全部通过 | 只声明记录范围内零 flaky；保留开发期失败及修正 |
| RE3 | 故障注入全绿 | F1A/F1B 原矩阵及独立隔离矩阵共 9 项，各 20 次通过 | 检查组合状态，不能只断言 Error |
| RE4 | 生命周期压力全绿 | F3 十二用例各 3 次，共 36 次逐项通过 | Workflow 批次持久化测试预算为 30 秒，其他默认 5 秒 |
| RE5 | 性能基线建立并记录 | F5C 新 21 报告、405 样本、60 生命周期，索引与 SHA-256 全部核对 | 程序摘要 5C5B3DCB…；保留全量复制成本，不外推业务 SLA/零泄漏 |

## F5 附加工程门禁

F5 附加门禁已经汇总：Production-only 的 31 工程无测试/Benchmark/ASan 工程、插桩配置或 CTest 文件；自有代码警告即错误；69 个公共头和 133 个生产源通过架构扫描及负例；ASan 的健康/真实错误探针与 264 项普通内核门禁共同通过；独立进程恢复各 20 次通过。三个探针没有替代真实内核全集，单次全集没有替代重复测试。日志、摘要和复现步骤见 F5C 记录。

## 明确不纳入本阶段

不实现 OCCT/OCAF/XDE、CAD/CAM、刀路、碰撞、运动学、控制器、MotionSafetyPermit、LMSI、GTN、Qt/Ribbon、产品 CLI parser、Python/Lua、RPC、AI、动态 Plugin Loader、Digital Twin 或 OpenTelemetry。若需要补项目级生命周期，也仅限内核状态、准入、持久化与恢复，不扩展项目 UI 或领域模块。
