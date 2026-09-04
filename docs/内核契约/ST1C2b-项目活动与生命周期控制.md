# ST1C2b 项目活动与生命周期控制

## 状态和范围

承接 C2a `2259c5a`，C2b 进行中，未签核。只完善内核准入、生命周期、持久身份和失效协议，不实现 CAD/CAM、Machine/Process、产品 CLI/RPC 或 GUI。

## 执行子节点

| 子节点 | 必须完成的内容 | 当前状态 |
| --- | --- | --- |
| C2b1 | Project-only Command/Query 直接活动租约；异步提交持有项目直到 Scheduler 接管；空项目关闭检查长期 Task，终态发布期间继续阻止关闭 | 本地检查点通过：Debug 320/320、专项 13/13、13 项各 10 次、纯生产/架构/文档通过；见 [C2b1 交付](../阶段交付/2026-09-04-ST1C2b1-项目活动与任务关闭桥接.md) |
| C2b2 | 生命周期控制与普通执行的明确分类，不能以改成 Global scope 绕过；Workflow/Script 长期归属与关闭/取消边界；legacy 持久身份恢复 | 进行中：[C2b2a 交付](../阶段交付/2026-09-04-ST1C2b2a-编排活动与关闭预检.md) 本地检查点通过，Debug 324/324、专项 20 项各 10 次及纯生产通过；下一步命令分类与恢复，C2b2 全项未签核 |
| C2b3 | CatalogRevision/epoch 的定义、失效触发与重启语义；不得把 open/close 当作 Project 业务内容提交，不得用墙钟时间冒充单调版本 | 待实施 |

这些子节点只是 C2b 的实施顺序，不改变完整验收范围。C2b1 通过不能将 C2b 标记为已完成；C2c drain/析构和 ST1D 最终认证仍独立保留。

## C2b1 约束与证据

1. AppKernel 内普通 Project scope 请求要求项目存在且 Open，即使项目没有任何文档。Command/Query 通过现有内核装配私有获取 ProjectActivityLease；保留 Capability、Schema、Scope 与幂等等既有检查，不新增 Host 可变准入口。
2. Document 请求仍通过文档租约间接持有项目；不重复计数同一个必要短期归属。Command/Query 的项目与文档活动保留到同步 trace/metrics 发布结束，拒绝/异常路径正确释放。
3. Task 的项目租约跨越准备、持久接受和 Scheduler 激活。提交返回后，关闭按 Scheduler 中真实 request.projectId 查询长期活动，不依赖“当前打开的子文档”来发现无文档任务。
4. 项目关闭先封闭新活动，再探测任务与既有子文档活动；被阻塞则保留 Open，不写入虚假的 Closed。其他项目不因该任务被无关阻塞。
5. Task 状态变成终态，但 completionReady 尚未完成时仍计入整体/文档/项目活动；观察发布、终态持久化及错误记录结束后才放行。CancelRequested 不是实际完成，重复取消不能放行。
6. 独立组件未装配 ProjectRuntime 时保留独立组件语义；这不代表 AppKernel 可绕过项目检查。Scheduler 和 TaskRuntime 增加只读 ProjectId 活动计数重载，无持久格式变化。
7. `ProjectLifecycleSnapshot.activities` 仅为短期租约数量，不是长期 Task 总数。关闭另外检查 Scheduler 项目活动；Task 接受失败须撤销 prepared 记录，任务执行器提交异常须完成失败终态，均不得留下活动泄漏。持久终态写入失败沿用既有诊断契约，本节点不承诺自动重试或把失败记录视为耐久成功。

生产修改前 `build/st1c2b1-red-tests.log` 为 0/3，退出 8，0.24 秒：关闭/不存在的项目进入 Handler、Handler/trace 中途关闭项目、无文档 Task 执行/取消/终态发布期间关闭项目均已复现。原始日志 SHA-256：`854FE192796FBAE719552B7F47A366BAF8BC9247B418466109B0644C77304615`。

扩展专项 `build/st1c2b1-fault-tests.log`：13/13、8.44 秒，含新增 6 项、C2a 6 项及既有 headless 观察兼容性用例。新增覆盖参数校验拒绝、Handler 异常、执行器提交异常、SQLite 接受写入错误/异常回滚和原请求重试、终态落库中的关闭拒绝及其他项目隔离。SQLite 故障用例位于基础设施测试目标，不向纯内核测试目标增加第三方依赖；使用独立 `:memory:` 数据库，不冒充重启或断电证据。

## 尚不能宣称完成的边界

- 现有公开 ProjectRuntime/DocumentRuntime 生命周期控制只取得 C2a 整体短期准入；普通 Project/Document Command 的活动保护不能随意跳过。命令化生命周期控制须明确契约和能力，不允许调用方提供 unchecked/skip 开关。
- Workflow/Script 以必需 DocumentId 关联项目。本轮补真实所有者校验及完整 advance/cancel 调用保护，终态 Workflow 的未落库检查点继续阻塞关闭。文档关闭先封闭内存准入并探测长期活动，确认无阻塞后才写 Closing，拒绝不产生 SQL；该局部检查点通过不替代命令化控制及重启归属验收。
- 旧迁移只从认证的 Journal/Snapshot/DocumentCatalog 提取项目根；历史 Project-only Task/Effect 等材料的关系尚须核验，不能读取未认证字符串直接造项目，也不能在迁移完成后自动修补缺根。
- Task 的真实执行器 drain 和 AppKernel 析构顺序属于 C2c。活动计数改进不证明物理安全、任意线程析构安全或设备准入。

## C2b2 实施与剩余验收顺序

1. C2b2a 编排活动与关闭预检：请求 ProjectId 必须拥有 DocumentId，拒绝不创建实例；在途 advance/cancel 覆盖节点回调、终态 trace/metrics 和取消检查点；终态未保存不能提前关闭，失败重试恢复；不要求已完成实例的只读/幂等清理控制重新打开容器。本地检查点通过，完整证据见 C2b2a 交付。
2. 生命周期命令分类：明确治理的控制操作、能力、目标身份及允许状态，close 不被自身普通执行租约阻塞，open 不要求目标已 Open；禁止调用方 skip/unchecked 或改成 Global 绕过。不得把直接 Runtime API 已有支持当作命令化控制已交付。
3. 持久归属与恢复：终态 Workflow 在文档 Detached、项目 Closed 和文档 Removed 后的恢复/历史查询本地检查点通过，见 [C2b2b 交付](../阶段交付/2026-09-04-ST1C2b2b-终态工作流历史恢复.md)：Debug 328/328、14 项各 10 次及纯生产通过；终态从认证的 DocumentCatalog（含 Removed 墓碑）核验所有者，非终态仍要求真实 Open 文档。缺失/错误归属、目录读取错误/异常均拒绝，不从 checkpoint 造项目或文档。legacy Project-only Task/Effect 的认证根仍待核验，不由此项代签。

生产修改前两项实例回归为 0/2、0.17 秒，证实错误 ProjectId 被接受及终态发布中提前关闭。扩展取消故障矩阵又暴露先写 Document Closing、后检查长期活动造成持久化回调重入锁和错误中间态，专项为 19/20；修复预检顺序后 20/20、7.54 秒。失败日志均保留，最终证据随本地交付登记。完成 C2b2a 不关闭后两项，也不关闭 C2b3/C2c/ST1D。

## 终态历史恢复约束（C2b2b）

- 终态属于历史，不要求当前容器可执行；只有已校验持久文档目录中的稳定归属可用于核验，包括 Removed 墓碑。不得用当前加载集合代替历史身份，也不得因历史存在而自动 open 或 recreate。
- 非终态恢复保持原 `Workflow.RecoveryDocumentUnavailable` 校验，不能利用历史分支进入 Detached/Closed/Removed 容器。终态缺失或错误目录归属返回 `Workflow.RecoveryOwnershipUnavailable`；读取错误沿原错误链传播，恢复实例表不部分安装。
- 保留工作流定义存在、版本、摘要、步骤集合及完成顺序校验；本轮不授予升级后的任意定义解释旧历史的权利，定义兼容性由 C6 完整限定。
- 终态快照不得转换 Running 步骤或设置重放标记；终态 advance/cancel 沿既有幂等终态路径返回，不进入 Handler。失败 Host 销毁后再重试，不在 Failed Host 原地强行重开。
- 六个自然终态场景（成功/取消 × Detached/Closed/Removed）生产修改前均重启失败；修复后每场景连续两次新 Host 恢复通过。其余 Failed/Compensated/CompensationFailed 通过认证检查点夹具验证状态保留，不等同于新增真实补偿执行测试。真实跨进程崩溃认证继续使用既有门禁，最终扩展由 ST1D 签核。
