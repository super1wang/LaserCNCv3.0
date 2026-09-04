# ST1C2 统一准入与生命周期收口

## 当前状态和问题证据

基于 C1b `80cd878`，C2a 已通过本地检查点，C2b/C2c 仍未签核。只覆盖内核，不扩展产品入口或领域模块。完整门禁见 [C2a 交付](../阶段交付/2026-09-04-ST1C2a-整体准入与停止线性化.md)。

C2a 修复前的 shutdown 先分散读取 Transaction/Command/Query/Workflow/Script 计数，再逐个 stop。执行入口的 accepting 检查与计数增加并不原子；Project/Document 生命周期操作未计入整体活动。Command/Query 计数还在最终 trace/metrics 发布之前释放。原有“Handler 已进入”并发用例不能证明这些窗口安全；下述共享准入门已补齐这些整体窗口。

首批回归利用既有基础设施边界：在 trace exporter 中重入 shutdown，以及在持久化操作开始时尝试 shutdown。`build/st1c2a-red-tests.log` 在生产改动之前 0/2、退出 8，4 类 Query scope 均在 trace 发布中被停成 Stopped；8 类生命周期分支中，7 类观察到中途成功停止，open 分支因中途停止而未能完成。不得向生产暴露测试专用暂停口，也不能以偶发压力全绿代替交错证明。

## C2a 实施约束

- AppKernel 持有一个私有共享准入门，公开执行入口和 Project/Document 生命周期写入口先取得短期活动租约，直到整个公开调用返回才释放，包括同步观察发布与错误路径。
- shutdown 的“活动为零并关闭准入”在同一同步临界区内完成。已有活动时继续返回拒绝并保持 Ready；成功关闭后不再允许新执行进入。保留原有活动事务优先错误和有界任务停止语义。
- Gateway 覆盖所有 scope，以及 Workflow/Script start、advance 和取消控制；内部 Command/Query/Task 提交在外层准入租约内完成，不能在外层返回后私自启动未登记工作。已提交异步 Task 的真实存续由 Scheduler 接管，不将提交完成等同于任务完成。
- 生命周期控制取得整体短期租约，但不自动取得目标的普通 Document/Project 活动租约。否则 close 会被自己阻塞，open 会错误要求目标已 Open。目标级活动与长期实例桥接在 C2b 验收。
- Workflow/Script 快照、Task 快照/等待/取消和 catalog 不取得执行准入，保留原有只读观察与 Task 停止控制行为；它们仍要求 Host 存活，不授予与析构并发的权利。Workflow/Script 取消会改变实例及持久检查点，取得完整短期准入；Stopping 后拒绝，不能在模块撤销后修改状态。
- 配置和 bootstrap 仍由 Host 生命周期线程调用；AppKernel 状态观察须避免数据竞争，重入/并发 shutdown 不得重复撤销模块或死锁。析构、非协作任务与依赖释放顺序由 C2c 完整验收。

## 必须取得的验收证据

1. 全部执行 scope、观察发布尾部、接受后登记前、Workflow/Script 初始实例和 advance，以及 Task 提交到 Scheduler 的交接。
2. Project create/open/close 与 Document create/attach/open/close/detach/remove 的持久化中间态，停止被拒绝且原操作可以完成。
3. 入口先取得租约和 shutdown 先关闭准入两种顺序；关闭后不执行 Handler、不新增实例/提交，不留下半状态。
4. 错误、抛异常、重入、并发停止和重复停止均能释放正确的租约；只读观察及停止控制保持明确行为。
5. C2b 的 Project-only 与长期活动、C2c 的 drain/析构必须独立核验，不能由 C2a 局部测试代签。

后续进展：C2b1 的 Project-only 与 Task 长期桥接已通过本地检查点；C2b2a 的编排实例归属、完整控制调用与关闭预检也已通过。C2b2 命令分类和恢复、C2b3 目录失效及 C2c 仍待完成，当前状态以 [C2b 契约](ST1C2b-项目活动与生命周期控制.md) 为准，文末 C2a 历史成绩不自动延伸为这些节点的签核。

## 当前实现和证据

私有 `src/kernel/execution_admission.hpp` 以互斥临界区同步租约计数与关闭，不在执行 Handler 或基础设施回调时持锁，不序列化普通请求。AppKernel 唯一持有准入门，Gateway 使用必需引用，Project/Document 使用仅由 AppKernel 安装的私有指针；独立组件没有默认启用的 Host 准入假象，也没有新增公共装配/重开接口。

租约为不可复制的 RAII 值，不增加共享堆分配；作用域退出与异常展开释放计数。原有 accepting 和局部计数仍用于组件契约、诊断及 Document close，不再承担整体线性化证明。`Kernel.ActiveTransactions` 保留优先检查；活动公开调用返回 `Kernel.ActiveExecutions`，准入保持开放。只有检查为空并关闭准入之后才进入 Stopping。

状态观察改为 atomic；bootstrap/shutdown 以生命周期调用标记排斥并发和同线程重入，冲突返回 `Kernel.LifecycleInProgress`，不等待回调所在调用结束而死锁。配置和销毁仍须由 Host 生命周期线程协调，不能由 atomic 状态推导任意对象终生线程安全。

首轮红转绿 2/2（10.13 秒）；扩展最终专项 6/6（8.88 秒），见 `build/st1c2a-second-tests.log`。覆盖 Command/Query 各四 scope、Workflow/Script 接受前校验与 advance 发布、Task Executor submit 交接、持久生命周期八分支和模块 stop 阻塞期间的并发/重入停止及入口拒绝。原始测试 `string_view` 链接问题、变量遮蔽警告和测试 Value 字面量歧义均保留编译日志，不作为生产红灯。

首轮完整 Debug 为 313/314、退出 8（597.33 秒）：Gateway 提前拒绝曾遗漏既有失败请求的 trace/metrics。已改为通过 Command/Query 私有观察包装强制拒绝，保留完整错误上下文与观察，不进入解析、Handler 或写状态；旧断言未降低。该中间全集不是最终代码证据。

最终 Debug 314/314（554.96 秒）、专项 7/7（9.09 秒）、7 项各 10 次共 70 次（83.44 秒）及纯生产/架构/文档门禁通过，形成 C2a 本地检查点。最终三配置、故障/真实进程恢复、连续压力、生产结构与新性能基线仍由 ST1D 统一重新签核。下一步 C2b 必须处理 Project-only 当前未取得项目活动租约，以及长期 Task/Workflow/Script 归属和目录 epoch；本门禁不替代目标级生命周期协议。
