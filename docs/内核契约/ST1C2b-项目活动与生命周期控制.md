# ST1C2b 项目活动与生命周期控制

## 状态和范围

承接 C2a `2259c5a`，C2b 进行中，未签核。只完善内核准入、生命周期、持久身份和失效协议，不实现 CAD/CAM、Machine/Process、产品 CLI/RPC 或 GUI。

## 执行子节点

| 子节点 | 必须完成的内容 | 当前状态 |
| --- | --- | --- |
| C2b1 | Project-only Command/Query 直接活动租约；异步提交持有项目直到 Scheduler 接管；空项目关闭检查长期 Task，终态发布期间继续阻止关闭 | 本地检查点通过：Debug 320/320、专项 13/13、13 项各 10 次、纯生产/架构/文档通过；见 [C2b1 交付](../阶段交付/2026-09-04-ST1C2b1-项目活动与任务关闭桥接.md) |
| C2b2 | 生命周期控制与普通执行的明确分类，不能以改成 Global scope 绕过；Workflow/Script 长期归属与关闭/取消边界；legacy 持久身份恢复 | C2b2a 编排活动、C2b2b 终态恢复、[C2b2c 固定命令](../阶段交付/2026-09-04-ST1C2b2c-受治理生命周期命令.md) 和 C2b2d 历史执行根认证均通过本地检查点，最新 Debug 347/347、8 项各 10 次及纯生产通过；下一步 C2b3，C2b 整体与 ST1D 尚未签核 |
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

## 持续约束与尚未签核的边界

- 公开 ProjectRuntime/DocumentRuntime 生命周期控制保留 C2a 整体短期准入；普通 Project/Document Command 的活动保护不能随意跳过。C2b2c 已实现固定命令的明确分类和能力检查，仍不允许调用方提供 unchecked/skip 开关；目录版本前置条件须由 C2b3 单独验收。
- Workflow/Script 以必需 DocumentId 关联项目。C2b2a 已补真实所有者校验及完整 advance/cancel 调用保护，终态 Workflow 的未落库检查点继续阻塞关闭。文档关闭先封闭内存准入并探测长期活动，确认无阻塞后才写 Closing，拒绝不产生 SQL；C2b2b 已补终态持久归属，仍不将局部门禁代替 ST1D 最终认证。
- C2b2d 已将认证的历史 Project-only Task/Effect 纳入原 Journal/Snapshot/DocumentCatalog 根集合，不能读取未认证字符串直接造项目，也不能在迁移完成后自动修补缺根。任意历史格式兼容、输入预算、历史保留与全量认证成本仍须由 C6/C7 限定。
- Task 的真实执行器 drain 和 AppKernel 析构顺序属于 C2c。活动计数改进不证明物理安全、任意线程析构安全或设备准入。

## C2b2 实施与剩余验收顺序

1. C2b2a 编排活动与关闭预检：请求 ProjectId 必须拥有 DocumentId，拒绝不创建实例；在途 advance/cancel 覆盖节点回调、终态 trace/metrics 和取消检查点；终态未保存不能提前关闭，失败重试恢复；不要求已完成实例的只读/幂等清理控制重新打开容器。本地检查点通过，完整证据见 C2b2a 交付。
2. C2b2c 生命周期命令分类：已实现固定操作注册、能力与目标状态检查；close 不持有自己的普通执行租约，open 不要求目标已 Open。本地检查点通过：最终 Debug 339/339、专项 25/25、11 项各 10 次及纯生产/架构通过，见 [C2b2c 交付](../阶段交付/2026-09-04-ST1C2b2c-受治理生命周期命令.md)。禁止调用方 skip/unchecked 或改成 Global 绕过，不接受自定义生命周期 Handler。不得把直接 Runtime API 已有支持当作命令化控制已交付。
3. 持久归属与恢复：终态 Workflow 在文档 Detached、项目 Closed 和文档 Removed 后的恢复/历史查询本地检查点通过，见 [C2b2b 交付](../阶段交付/2026-09-04-ST1C2b2b-终态工作流历史恢复.md)：Debug 328/328、14 项各 10 次及纯生产通过；终态从认证的 DocumentCatalog（含 Removed 墓碑）核验所有者，非终态仍要求真实 Open 文档。缺失/错误归属、目录读取错误/异常均拒绝，不从 checkpoint 造项目或文档。legacy Project-only Task/Effect 的认证根单独由下述 C2b2d 验收，不由 Workflow 历史测试代签。

生产修改前两项实例回归为 0/2、0.17 秒，证实错误 ProjectId 被接受及终态发布中提前关闭。扩展取消故障矩阵又暴露先写 Document Closing、后检查长期活动造成持久化回调重入锁和错误中间态，专项为 19/20；修复预检顺序后 20/20、7.54 秒。失败日志均保留，最终证据随本地交付登记。完成 C2b2a 不关闭后两项，也不关闭 C2b3/C2c/ST1D。

## 终态历史恢复约束（C2b2b）

- 终态属于历史，不要求当前容器可执行；只有已校验持久文档目录中的稳定归属可用于核验，包括 Removed 墓碑。不得用当前加载集合代替历史身份，也不得因历史存在而自动 open 或 recreate。
- 非终态恢复保持原 `Workflow.RecoveryDocumentUnavailable` 校验，不能利用历史分支进入 Detached/Closed/Removed 容器。终态缺失或错误目录归属返回 `Workflow.RecoveryOwnershipUnavailable`；读取错误沿原错误链传播，恢复实例表不部分安装。
- 保留工作流定义存在、版本、摘要、步骤集合及完成顺序校验；本轮不授予升级后的任意定义解释旧历史的权利，定义兼容性由 C6 完整限定。
- 终态快照不得转换 Running 步骤或设置重放标记；终态 advance/cancel 沿既有幂等终态路径返回，不进入 Handler。失败 Host 销毁后再重试，不在 Failed Host 原地强行重开。
- 六个自然终态场景（成功/取消 × Detached/Closed/Removed）生产修改前均重启失败；修复后每场景连续两次新 Host 恢复通过。其余 Failed/Compensated/CompensationFailed 通过认证检查点夹具验证状态保留，不等同于新增真实补偿执行测试。真实跨进程崩溃认证继续使用既有门禁，最终扩展由 ST1D 签核。

## 固定生命周期命令契约（C2b2c）

模块通过 `ModuleRegistrar::registerLifecycleCommand(CommandDescriptor)` 注册固定 `LifecycleOperation`，仍须在模块的 commands 清单声明精确名称/版本，失败污染注册结果并撤销贡献，bootstrap 后冻结。内核不默认安装命令模块或授予能力；命令名称和 CapabilityId 由可信模块注册，运行会话须显式获授权。普通 Handler 的注册通道拒绝 lifecycleOperation 元数据。

| 固定操作 | Scope 与目标 | 前提和结果 |
| --- | --- | --- |
| ProjectCreate | ProjectId / Project | 新项目 → Open |
| ProjectOpen | ProjectId / Project | Closed → Open，不自动打开子文档 |
| ProjectClose | ProjectId / Project | Open 且没有真实阻塞活动 → Closed，按既有协议关闭子文档 |
| DocumentCreate | ProjectId + DocumentId / Document | 父项目 Open，内存身份未占用且认证持久目录无同 ID → Open |
| DocumentOpen | ProjectId + DocumentId / Document | 同一所有者、父项目 Open、持久 Detached → Open；无持久化时拒绝 |
| DocumentClose | ProjectId + DocumentId / Document | 同一所有者、父项目 Open、文档 Open 且无真实阻塞活动 → Detached |
| DocumentRemove | ProjectId + DocumentId / Document | 同一所有者、父项目 Open、Detached → 持久 Removed 墓碑，移出运行目录 |

- 元数据只允许 Synchronous + LifecycleControl，匹配操作的精确 Scope、Object 参数/结果根类型，不允许 undoable、idempotent、外部资源/Guard 或 ReplayPolicy 非 Never。deterministic 不赋予重放权。
- 请求参数必须为空 Object；目标仅来自 ExecutionContext，调用方不能指定操作枚举、第二目标或 skip。保留现有版本解析、Scope、Schema、Capability 检查。业务 expectedRevision 与 idempotencyKey 显式拒绝；目录版本前置条件仍由 C2b3 定义，当前不伪造支持。
- 控制命令不获取自身目标的普通 Command 活动租约，但保留整体 Kernel 准入至 trace/metrics 发布结束，并通过原 Runtime 状态机检查其他短期/长期活动；不新增第二套事务或外部副作用执行路径。文档控制持有请求父项目的短期租约，close/remove 在状态转换锁内用该租约再次匹配所有者；open 匹配认证目录中的请求所有者，不仅依赖前置查询，相关辅助入口保持私有。返回 projectId、可选 documentId 和 state，无业务 commit/Task/replayed。
- 转换成功后结果 Schema/日志故障属于 postExecutionErrors，不把已完成转换报告成未执行；持久化转换失败仍返回失败并保留 Runtime 的真实 Failed/局部状态，不承诺多文档关闭原子回滚。
- 新回归发现 create 可复用 Removed 墓碑及 Detached 身份。现在 Runtime create 先预留 Opening 身份，再读取认证目录，拒绝/读取故障撤销未写入的预留；拒绝不写 Closing/Opening 或产生新可执行文档。无持久化的 remove 仅移除内存记录，不承诺跨进程墓碑。公开 attach、配置期装载及 Journal/Snapshot 单独来源的完整身份域仍由 C3/C4 验收，不能借本修复代签。
- 本节点新增全目录认证读取，未宣称常数时间创建；定向查询及容量成本纳入 C7 实测。持久格式未改变；公共枚举/Descriptor 新增字段影响源码编译，C6 清单须登记，不承诺跨工具链二进制 ABI。

## C2b2d：legacy 根认证

本节点通过本地检查点：最终 Debug 347/347、专项 8/8、8 项各 10 次与纯生产/架构通过，见 [C2b2d 交付](../阶段交付/2026-09-04-ST1C2b2d-历史执行项目根认证.md)。以下保留本节点原验收依据和约束；C2b3、C2c 及后续冻结节点继续执行。

实施前 AppKernel 只从认证文档镜像/目录汇集项目根，迁移写事务也只比对 document_catalog、state_journal、snapshot_index。Task/Effect 原有按 ID 查询不能替代完整历史根枚举。本轮同时接入根认证、一次性集合核验和迁移后的缺根拒绝，没有只给启动路径拼接几个 ProjectId。

1. 对只有 Project-only Task 或 Effect 历史、没有任何 Document 的旧库补先失败回归；Global/Session 历史不得合成项目。
2. 根必须来自已核验受支持格式、结构、摘要及身份一致性的真实请求/签名；畸形载荷、被篡改摘要和缺失归属拒绝。不得只信 SQL 控制列或从任意 JSON 文本递归搜 projectId。现有 taskHistory 主要核验任务/Trace/修订等字段，不能把其成功返回视为 projectId/documentId 已认证；须补明确的归属解析。Task 的 lasercnc.task-acceptance 与 Effect 的 lasercnc.external-effect-signature 各有 format/version，须分别按真实既有编码验证，不擅自修改旧格式。
3. pending 迁移一次性核验完整去重集合；迁移完成后缺根必须拒绝，不在每次启动自动造项目修补。失败/异常不部分安装运行目录，原材料保留。
4. 覆盖同进程新 Host 再恢复和故障重试，明确终态历史只提供身份依据、不授予执行/重放权限。独立进程和最终三配置仍在 ST1D 统一认证。

## 下一执行节点：C2b3 目录版本与失效

1. 先明确目录视图的身份与作用域：Project/Document 生命周期目录不等同于 Gateway 中的模块/命令注册目录，也不等同于 Project 业务修订。版本和值须构成一致快照，不能分别查询后拼接出不存在的状态。
2. 定义 create/open/close/remove、恢复安装、失败中间态和无变化重试的失效规则；短期活动计数变化是否属于目录内容须明确，不能隐式把每次查询自身准入都变成目录更新。
3. 明确 epoch 与单调修订的职责、Host 重建后的旧令牌失效、溢出处理及持久/非持久配置的支持边界；不得用 updatedAt 或墙钟时间冒充版本，也不得把仅进程内计数宣称为跨重启稳定版本。
4. 先补陈旧视图、读写交错、部分关闭失败、新 Host、跨项目隔离与无业务提交回归，再接入实现；如增加生命周期前置条件，检查与目标状态转换须属于同一受保护步骤，禁止先比较后无条件写入。
5. C2b3 不扩展 UI 缓存或产品订阅模块；公开 DTO/版本令牌变更登记到 C6，成本进入 C7，独立进程和三配置最终认证保留在 ST1D。C2b 全项在这些语义与证据闭合前不签核。
