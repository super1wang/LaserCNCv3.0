# Kernel 1.0 冻结逐项审计清单

## 当前结论

尚未冻结。本表逐项承接《Kernel 1.0 最终收口设计规划》第 5 节，列出待终审的实现与测试证据入口；入口存在或历史阶段已验收，不自动等于最终版本已满足条件。F5A 固定版本已通过 ASan 262/262、Debug/Release 各 259/259、Production-only 与架构门禁，但 Host 状态写入口、独立项目生命周期范围、最终连续重复矩阵和本表逐项签核尚未闭合。

## 需要先明确的验收范围

最终清单的 State 第一项写明“Project/Document 可以运行期创建、打开、关闭”；K10B 细则则只指定 DocumentRuntime 的 create/attach/open/snapshot/close/detach/remove/list，以及 Project ownership 冲突。当前代码只有后者：Document 归属 ProjectId，Project Revision 按项目共享，没有独立 ProjectRuntime、ProjectLifecycle 或项目级 create/open/close。

因此当前证据不能证明独立项目生命周期已完成。已向用户提出范围确认：补齐项目级内核生命周期（不做项目 UI），或明确本阶段按 K10B 细则验收文档生命周期与项目所属关系。在明确前，该项保持未闭合；不能通过改写“已验收”的定义绕过它。

## 第 5 节逐项证据索引

以下表格为审计索引，不是最终通过声明。终审需同时核对源码/断言及最终版本的实际测试记录。

补充发现：Host 仍能通过可变 History/Persistence 调用底层状态写接口。该问题与 Project 范围确认独立，须修复后再签核 TX3/ET4；具体可达路径和后续门禁见 [F5B Host 状态写入口审计](K10F-F5B-Host状态写入口审计.md)。

| 编号 | 规划要求 | 当前实现与证据入口 | 终审注意点 |
| --- | --- | --- | --- |
| EX1 | Command/Query 请求版本化 | command/query.hpp、对应 Registry；command_query_runtime_tests 的 compatible/deprecated/version 用例 | Exact/Compatible、拒绝与响应解析版本均需覆盖 |
| EX2 | 四类 Execution scope 完整 | execution_scope、Command/Query Runtime；四 scope 用例及异步 Global Task 用例 | 无文档 scope 不应取得文档快照 |
| EX3 | ReadOnly/DocumentWrite/ExternalEffect 语义闭环 | CommandRuntime、EffectExecutor、TaskRuntime；执行链及故障用例 | 不让只读/外部副作用走普通文档 Undo |
| EX4 | External recovery/replay policy | effect_executor、effect_persistence；F2B Safe/Idempotent/ReconcileOnly/Never | 仅显式允许重试，不自动调用 handler |
| ST1 | Project/Document 运行期 create/open/close | DocumentRuntime、document_state_tests、持久目录用例 | **项目级含义未闭合，见范围确认** |
| ST2 | Document 生命周期与 Task/Transaction 一致 | DocumentActivityLease、CloseBlockers；F3 Query/Task/Workflow/关闭压力 | 活动准入和长期工作检查不能有空窗 |
| ST3 | Revision 冲突 fail-closed | RevisionManager、TransactionManager；版本冲突及八路竞争用例 | 唯一持久胜者，失败候选无状态残留 |
| TX1 | DocumentWrite 唯一 Transaction 链 | AppKernel 私有 transactions_、CommandRuntime、Workflow/Script 调度 | AppKernel 不公开可变存储/事务管理器 |
| TX2 | Undo/Redo 正式可用 | HistoryRuntime、内置 edit.undo/redo；history_runtime_tests | 覆盖变更形态、分支与 barrier，不仅单次返回值 |
| TX3 | Journal/History/Revision 一致 | TransactionManager + PersistenceService；F1A/F2A/F3 | 合法执行链已覆盖；Host 可变 History.restore 旁路待封闭 |
| ET1 | ModuleRegistrar | module_registrar.hpp/cpp；声明/实际贡献用例 | 首次被忽略的注册错误也必须使启动失败 |
| ET2 | Registry ownership audit | ModuleRuntime、ModuleContributionSnapshot；九类模块贡献/回滚用例 | Event/Capability 是贡献审计事实；安全 Guard 是 Kernel 配置表，不冒充新增领域 Registry |
| ET3 | Registry Ready 后冻结 | AppKernel bootstrap；各 Registry frozen 与配置拒绝用例 | 包括 ObjectType 与 EffectGuard；不同 Registry 的负责者须明确 |
| ET4 | Host 无直接 Task/Transaction 旁路 | AppKernel、ExecutionGateway、Scheduler 私有 schedule；类型断言与架构扫描 | Gateway 无提交接口；Host 可变 Persistence 底层写入口仍待收紧 |
| PE1 | Idempotency 跨重启 | PersistenceService、CommandRuntime；持久幂等与 F2A | 签名绑定版本、scope 与请求内容 |
| PE2 | Snapshot/Journal crash-safe | SQLite/Snapshot 适配器、恢复链；F1/F2 独立进程 | 软件进程终止范围，不扩大为物理掉电证明 |
| PE3 | Workflow recovery | workflow_persistence、WorkflowRuntime；F2B 三进程与 checkpoint 故障 | 固定 attempt，不自动重放不安全外部副作用 |
| PE4 | History recovery | HistoryRuntime.restore；F2A Undo/Redo 中断恢复 | 游标和前后像一致，恢复后实际可用 |
| PE5 | Asset publish crash-safe | FilesystemAssetStore；F2C 发布与引用故障 | 已提交资产丢失/损坏拒绝恢复 |
| SA1 | Capability 默认拒绝 | CapabilityService；Command/Query/Effect 前置拒绝用例 | 注册 capability 不等于授予会话权限 |
| SA2 | 外部副作用不自动重放 | EffectExecutor recover、Workflow 恢复；F2B | 使用独立调用记录确认没有隐含 handler 执行 |
| SA3 | Unknown fail-closed | 枚举/持久 wire 校验；篡改状态/策略/错误材料用例 | 缺失、无效、过深或不一致材料拒绝安装 |
| SA4 | 不做 Machine-specific safety 判断 | Kernel 目录/公共头、架构扫描和 K10E-E3 | 不等于物理安全认证，不实现控制器/碰撞/运动许可 |
| SA5 | 统一 Effect Guard | IEffectGuard、EffectGuardRegistry、EffectExecutor；Guard 次序与拒绝用例 | Ready 冻结，缺少声明 guard 时启动拒绝 |
| DA1 | ObjectType version/migration | ObjectTypeRegistry、版本化 ObjectRecord；迁移与恢复准入用例 | 显式迁移、完整候选/引用验证，失败无半迁移 |
| DA2 | AssetRef/Data Plane | AssetRef、IAssetStore、资产状态测试和 F2C | 大资产在文件侧，历史前像引用也验证 |
| DA3 | 第三方类型不进入 Kernel API | VerifyKernelBoundaries、OCCT 负例 | 正扫描与反例同跑，不能只看链接成功 |
| RE1 | Debug/Release 全绿 | F5A 固定版本双配置各 259/259 | F5B 修改入口后必须重新固定版本并重跑 |
| RE2 | 无 flaky test | 最终连续重复矩阵 | 只声明记录范围内零 flaky；保留开发期失败及修正 |
| RE3 | 故障注入全绿 | F1A/F1B 矩阵及最终专项 | 检查组合状态，不能只断言 Error |
| RE4 | 生命周期压力全绿 | F3 十二用例，固定参与者/轮次/同步序 | Workflow 批次持久化测试预算已改为 30 秒，其他默认 5 秒 |
| RE5 | 性能基线建立并记录 | F4 原始 21 报告、索引、SHA-256 与结构判断 | 当前普通 Release 基准二进制摘要与归档一致；不外推业务 SLA |

## F5 附加工程门禁

最终还须汇总 Production-only 构建及目标隔离、编译警告即错误、架构扫描与负例、可用 ASan 配置/真实探针/内核全集、独立进程恢复重复执行。三个 ASan 预期失败/健康探针不能替代真实内核全集，单次全集也不能替代重复测试。

## 明确不纳入本阶段

不实现 OCCT/OCAF/XDE、CAD/CAM、刀路、碰撞、运动学、控制器、MotionSafetyPermit、LMSI、GTN、Qt/Ribbon、产品 CLI parser、Python/Lua、RPC、AI、动态 Plugin Loader、Digital Twin 或 OpenTelemetry。若需要补项目级生命周期，也仅限内核状态、准入、持久化与恢复，不扩展项目 UI 或领域模块。
