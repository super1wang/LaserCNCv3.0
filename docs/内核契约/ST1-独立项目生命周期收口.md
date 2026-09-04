# ST1 独立项目生命周期收口

## 依据与状态

ST1 整体实施中，尚未验收；ST1A 持久组件检查点已通过下述 Debug 和纯生产门禁。重新核对原规划：总览第 38 行将运行期 Project/Document 生命周期列为必须完成，K10B 标题明确包含 Project，最终 State 清单要求两者均可运行期创建、打开、关闭。因此按既有用户目标补齐独立项目内核能力，不再将“是否实现”视为前置阻塞；先前的范围问询及 F5C 未冻结记录保留为历史证据。这里不是把 K10B 已有的 Document ownership 重新命名为项目生命周期。

本阶段只做内核身份、准入、持久化与恢复，不做 Project UI、文件对话框、布局恢复、CAD/CAM 或其他领域模块。

## 完整目标契约

1. AppKernel 唯一拥有 ProjectRuntime，项目可以没有文档；提供 create/open/close/lifecycle/list，配置期可显式声明项目。稳定 ProjectId 不因关闭而消失，不新增任意删除持久材料的入口。
2. 状态为 Closed → Opening → Open → Closing → Closed；转换不确定或失败进入 Failed。只有 Open 可接受新的项目/文档活动；诊断读取仍可用，未知项目和未知状态拒绝。
3. 项目关闭先在同一互斥边界封住新准入，再检查 Command/Query 租约、Transaction、Task、Workflow、Script。阻塞时不关闭任何子文档。通过检查后协调关闭所属 Open 文档；不能仅因为存在文档就把整个操作永久定义为不支持。
4. 项目关闭不是跨多个文件快照的原子事务。若部分文档已关闭、后续持久化失败，项目保持 Failed，保留各文档的真实结果，不伪称全部 Closed，也不恢复为可执行的 Open。Opening/Closing 的进程中断按 Failed 恢复，不自动猜测完成。
5. open 重新开放项目容器及其目录，不自动打开所有文档或重放任务；Document 仍通过明确的 open 恢复所需镜像。项目关闭后不能通过文档 create/attach/open、Project scope Command/Query、Task/Workflow/Script 绕过项目状态。
6. 已有 Configuring addDocument 隐含的所属项目作为显式启动组合的一部分兼容迁移；运行期不能凭任意 ProjectId 静默创建项目。旧 Journal/Snapshot/Document catalog 的真实项目身份通过一次性迁移建立目录，迁移完成后缺失目录不再自动补造。
7. Project 生命周期元数据采用版本化持久目录，不创建第二条文档 Commit/Undo/Redo 链；Project Revision 继续属于现有 Revision 模型，不把打开/关闭冒充业务文档变更。

## 分节点实施与验收

2026-09-04 交叉审计后，剩余顺序及独立缺陷门禁以 [ST1C 补充执行计划](ST1C-补充审计与剩余执行计划.md) 为准。ST1C 不再仅指项目存在性租约，还包括独立修订恢复、Kernel 停止/析构、存储键、可信装载、单 Host 与冻结契约。先执行 C1a 非零 ProjectRevision 重启回归；全部必须项闭合后进入 ST1D。

- ST1A：schema v9 的 Project catalog、版本化/摘要绑定的状态、一次性迁移标记与回滚；空项目持久化、篡改、未知状态、迁移失败与中断状态测试。此节点只提供持久化基础，不宣称已具备 ProjectRuntime。
- ST1B：AppKernel 所有权、ProjectRuntime 状态机、Document 生命周期联动和项目关闭协调；内存/SQLite/多文档/空项目测试。
- ST1C：Project/Document scope 的统一租约、Task/Workflow/Script 长期活动桥接、启动恢复与 legacy 准入；明确交错和失败状态断言。
- ST1D：故障矩阵、真实三进程中断恢复、连续压力、三配置全集、纯生产和最终基线重测；重新逐项签核冻结清单。

后一个节点必须承接前一个节点的真实 API 和存储格式。ST1A 的组件测试不替代 B/C/D，F5C 的旧二进制基线也不代表新增项目能力后的最终版本。

## ST1A 持久化设计

`project_catalog` 保存 ProjectId、state、带格式标识 `lasercnc.project-lifecycle.v1` 的 payload、ContentDigest、更新时间；payload 同时绑定身份、状态与时间。只接受 Closed/Opening/Open/Closing/Failed，读取转换中状态时返回 Failed 和 interruptedTransition，不暗中回写。

schema v9 建立单独的一次性迁移标记。初始化 v8 及更早数据库只建立表和 pending 标记，不将 SQL 列当成已验证业务状态。完成迁移必须由 Kernel 在旧恢复材料通过校验后提交项目集合，且集合与 Journal/Snapshot/Document catalog 的全部所属身份一致；所有新项目记录与标记在同一数据库事务提交。迁移完成后重复调用不添加新项目，标记/表缺失或不合法拒绝，不能通过重启把缺失状态“修复”为初始空目录。

项目保存与目录读取在迁移完成前拒绝。独立 PersistenceService 组件保留装配和低层写入能力；AppKernel 的 const Persistence 观察边界继续禁止 Host 调用迁移/保存方法。后续 Kernel 启动必须接入这一迁移步骤，ST1A 尚不满足该启动集成要求。

迁移已经完成时，completeProjectCatalogMigration 是不添加身份的幂等空操作，不表示其参数已经获准创建项目。每次 Kernel 启动仍必须把全部已验证文档所属身份与现有项目目录逐一核对，缺失时拒绝，不能用 saveProjectLifecycle 补造。迁移源只承接真实文档根身份；只有 ProjectId 上下文、却没有项目或文档根身份的旧执行材料，不能据请求字符串自动创造项目，后续恢复准入须显式处理这一情况。

项目关闭协调只能使用内核私有的关闭通道，不能增加 Host 可传入的 skipPolicy 或可变存储后门。关闭文档继续遵守既有内存/持久化规则，不另建项目级镜像存储或第二个状态真相；项目 open 也不暗中恢复 UI 或所有文档。

## ST1A 开发期验证

生产代码首轮编译通过，但新测试直接比较 string_view 错误码时触发 Catch2 格式化符号链接不匹配；改用仓库既有的 std::string 显式转换，不修改 Catch2 或生产契约。原日志 `build/st1a-debug-build.log` 保留。

修正并补齐 Journal-only/Snapshot-only 来源及读写序列化故障后，专项 12/12 通过（26.54 秒），日志 `build/st1a-focused-tests.log`，构建日志 `build/st1a-debug-fixed-build.log`。包含 11 项新 Project catalog 用例及更新为拒绝 schema 10 的既有未来版本用例。另有普通/const Host 不可调用项目保存和迁移的编译期断言。

四项新故障矩阵分别覆盖数据库/摘要的迁移与更新（26 场景）、回滚返回错误/异常隔离（4 场景）、目录读取故障（4 场景）、序列化与反序列化返回错误/异常（4 场景）。十种篡改包括重算摘要后的错误格式版本及缺失字段；均检查失败后原始行未被覆盖。测试现场保留在构建目录内，不删除用户数据。

## ST1A 本地组件检查点

生产与测试代码固定后顺序完成以下门禁，最后只更新文档：

| 门禁 | 结果 | 日志 |
| --- | --- | --- |
| 完整 Debug | 275/275，326.37 秒 | `build/st1a-debug-full-tests.log` |
| 专项连续重复 | 12 项各 3 次，共 36 次，70.14 秒 | `build/st1a-focused-repeat3.log` |
| Production-only Release | 31 工程，0 测试/contract/Benchmark/ASan 工程、0 插桩配置、0 CTest 文件 | `build/st1a-production.log` |
| 架构扫描 | 69 公共头、134 生产源通过；全集包含正负例 | 同上及全集日志 |

所有成功记录按用例编号分组，核对唯一用例数及每组实际次数。全集日志 SHA-256 为 `E076638BE3B06B7D5A325BB9CCD5D21A6AF8E5DF7704FFD93B20E28388CB97CF`；专项重复为 `66D21655E82994FF6A925011E99881F2F3AEF0D9397F48598257B8BE49520649`。自有代码警告即错误；完整构建日志 `build/st1a-debug-full-build.log`。

此节点只验收持久组件和向后兼容的旧执行链回归。AppKernel 尚未接入项目目录迁移和项目生命周期，不能将 schema v9 存在解释为 ProjectRuntime 已完成；启动准入、关闭协调、跨进程项目中断和完整 Release/ASan/最终重复矩阵均由 ST1B/C/D 继续完成。当前普通 Release Benchmark 仍属于 F5C 的历史版本，不代表新代码。见 [ST1A 交付记录](../阶段交付/2026-09-04-ST1A-项目持久目录与迁移基础.md)。

## ST1B 本地集成检查点与后续边界

ST1B 本地集成检查点已通过。AppKernel 已持有 ProjectRuntime，新增配置期 `addProject` 和运行期 create/open/close/lifecycle/list；原配置期 addDocument 显式建立所属项目，运行期 create/attach 不再隐式建立项目。空项目、关闭后的稳定身份独立于文档目录存在。

项目状态机先在内存中封住准入，然后执行持久转换。关闭先检查全部子文档及其既有长期活动阻塞器，通过后才持久化 Closing 并按稳定文档身份顺序协调 close。阻塞预检不关闭任何子文档；持久化或子文档关闭失败后项目 Failed，保留各子文档真实状态。内部协调方法私有，Host 无跳过项目策略的开关。

文档 create/attach/open/close/detach/remove 和现有 Document activity 租约已绑定父项目准入；租约分配在加锁前完成，计数在确认 Open 后建立，释放顺序为文档再项目。文档身份在准入与执行之间重新绑定时还会二次核对父项目身份。Project-only scope 尚需 ST1C 统一接入，当前不能据此宣称全部项目执行边界关闭。

启动恢复先验证旧文档与历史材料，再提交一次性迁移；之后每次都验证持久根身份确实存在。显式配置不能修复已迁移目录中缺失的根，也不能自动重开持久 Closed/Failed 项目。空的中断项目以 Failed 诊断状态装载且不回写证据；不可用项目下存在持久 Open 文档时，当前拒绝 Kernel 启动，不将其伪装为安全打开。更完整的 legacy 执行恢复及停止交错由 ST1C 继续审查。

验证包含内存/SQLite 空项目、多文档关闭与明确打开、缺失目录不修复、中断与不一致父子状态拒绝、部分关闭失败、两次生命周期写入故障，以及真实文件快照阻塞期间的准入交错。开发期构建断言错误日志保留。最终专项 23/23（91.32 秒）、完整 Debug 284/284（449.81 秒）、ProjectRuntime 9 项各 3 次共 27 次（219.89 秒）及纯生产 Release 通过。架构检查为 70 公共头、136 生产源；纯生产 31 工程且无测试/Benchmark/ASan 工程、无插桩、无 CTest 文件。完整日志及 SHA-256 见 [ST1B 交付](../阶段交付/2026-09-04-ST1B-项目生命周期与文档关闭协调.md)。ST1B 不替代 ST1C、ST1D，也不发布 Kernel Frozen。

ST1D 的最终重复清单必须纳入新增 `[project-runtime][concurrency]` 和新增 `[fault-matrix]` 用例，不能机械复用 F5C 的旧命名正则或旧用例数。ST1B 自身的关闭交错每次包含 20 轮真实 SQLite/快照操作；完整门禁仍需基于最终代码重新执行。
