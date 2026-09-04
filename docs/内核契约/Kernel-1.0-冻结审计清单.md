# Kernel 1.0 冻结逐项审计清单

## 当前工作状态：ST1 补齐中

2026-09-04 用户已确认按交叉审计补齐计划。当前状态见 [ST1C 补充执行计划](ST1C-补充审计与剩余执行计划.md)：C6b21 已完成公共契约最终对账，[C6c1](ST1C6c1-共享Value与序列化预算.md) 已建立共享 Value、Schema 与 JSON/TOML 第一批硬预算，并完成 Release 全量 498/498、ASan 焦点三轮、纯生产及 71/141 边界。下一活动节点为 C6c2；C6c/d、C7/C8、ST1D 仍未签核。历史检查点不自动覆盖新增实现，C6c1 通过不是 C6c 或完整冻结，历史 31/32 不表示只剩一项。

C1a 已修复全部文档 Detached/Removed 后重启丢失 ProjectRevision 的缺陷；旧代码先复现失败，修复后 Debug 286/286、专项 16/16、新增 2 项各 3 次、纯生产及架构检查通过，见 [C1a 交付](../阶段交付/2026-09-04-ST1C1a-项目修订独立恢复.md)。该本地检查点不关闭 C1b/C5 写入端与独占权，也不替代 ST1D 最终签核。

复核原规划总览、K10B 标题和 State 清单后，独立 Project 生命周期被确认为原目标内的必需能力，不再作为额外范围决策阻塞。ST1A 的持久组件以及 ST1B 的 ProjectRuntime、文档联动和关闭协调均已通过本地检查点。Project-only 执行准入、完整恢复与最终认证仍未签核，不能宣布 Frozen；详见 [ST1 收口契约](ST1-独立项目生命周期收口.md)。

ST1 将影响状态、执行、持久化和恢复；以下 31 项通过记录属于 F5C 固定版本，不自动延伸到新增代码。所有受影响项须在 ST1D 完成三配置、故障/恢复/压力与新基线后重新签核，当前仍不得宣布 Frozen。

C5a/b 已形成会话端口、实际文件独占和初始化强制准入的本地检查点，见 [C5b 交付](../阶段交付/2026-09-04-ST1C5b-初始化独占与活动状态保护.md)：Debug 299/299、专项 56/56、新增 5 项各 10 次及纯生产/架构通过。第二 Kernel 不改写活动 claim 的原始回归红转绿，跨进程拒绝与接管、策略漂移和回滚隔离后保锁均已验证。C5 的最终签核仍与 C2c drain/析构、支持矩阵和 ST1D 联动，不以本地检查点代替整体冻结。

C1b 已在独占连接写事务内核验持久修订、合法变化和首笔 Catalog/Snapshot 归属，见 [C1b 交付](../阶段交付/2026-09-04-ST1C1b-Journal写入修订一致性.md)：Debug 308/308、专项 14/14、新增 9 项与扩展故障矩阵各 10 次通过。实际事件暂存、零交付和重启成功交付均有断言，失败不发布对象、修订、History 或成功回执。下一步 C2；正式容量、三配置认证和整体 Frozen 仍须 ST1D 重新签核。

C2a 已补齐整体准入和停止协调，见 [C2a 交付](../阶段交付/2026-09-04-ST1C2a-整体准入与停止线性化.md)：Debug 314/314、专项 7/7、7 项各 10 次通过。原始生命周期/观察发布中途停止已红转绿，首轮遗漏失败 trace/metrics 的回退也已修复且保留日志。下一步 C2b/C2c；Project-only 活动、目录失效、非协作任务 drain/析构及 ST1D 仍未签核。

C2b1 已完成普通 Project-only 执行、无文档 Task 长期桥接及终态发布中的关闭保护，见 [C2b1 交付](../阶段交付/2026-09-04-ST1C2b1-项目活动与任务关闭桥接.md)：Debug 320/320、专项 13/13、13 项各 10 次及纯生产/架构通过。原始 3 项红灯已转绿，接受写入失败/异常、执行器异常和项目隔离均已验证。下一步 C2b2/3；生命周期控制分类、Workflow/Script 归属、legacy 恢复、目录失效、C2c 和 ST1D 仍未签核，不能以此宣布 Frozen。

C2b2a 已修复 Workflow/Script 错误项目归属接纳、终态发布期间提前关闭，以及取消检查点回调中关闭文档先写 Closing 引发的重入问题。见 [C2b2a 交付](../阶段交付/2026-09-04-ST1C2b2a-编排活动与关闭预检.md)：Debug 324/324、20 项各 10 次和纯生产/架构通过；拒绝关闭无 SQL、未保存终态阻塞及重试成功均有断言。生命周期命令分类、终态/legacy 恢复仍在 C2b2 待办，C2b3/C2c/ST1D 仍未签核。

C2b2b 已修复关闭/删除容器后的终态 Workflow 恢复失败，见 [C2b2b 交付](../阶段交付/2026-09-04-ST1C2b2b-终态工作流历史恢复.md)：Debug 328/328、14 项各 10 次与纯生产/架构通过。六个自然场景连续两次新 Host 恢复、五种终态保留、缺失/错误归属、非终态关闭拒绝和目录读取故障均有断言；新测试不冒充独立进程或断电认证。生命周期命令分类、legacy Project-only Task/Effect 认证根、C2b3/C2c/ST1D 仍未签核。

C2b2c 本地检查点通过：固定生命周期命令的治理、权限、Scope、版本、目标状态及关闭活动检查已经实现，并修复 create 复用持久墓碑的问题；最终 Debug 339/339、专项 25/25、新增 11 项各 10 次和纯生产/架构通过，见 [C2b2c 交付](../阶段交付/2026-09-04-ST1C2b2c-受治理生命周期命令.md)。该进展不关闭 legacy Project-only Task/Effect 根、C2b3/C2c、C3/C4/C6–C8 或 ST1D；状态仍是 ST1 补齐中。

C2b2d 本地检查点通过：Task/Effect 历史根已进入认证、一次性迁移和启动缺根校验，不重放历史、不自动补文档；最终 Debug 347/347、专项 8/8、新增 8 项各 10 次和纯生产/架构通过，见 [C2b2d 交付](../阶段交付/2026-09-04-ST1C2b2d-历史执行项目根认证.md)。上述为分节点历史证据，当前下一步为 C2b3；C2c、C3/C4/C6–C8 和最终 ST1D 仍未闭合。

C2b3 本地检查点通过：独立于业务 Revision 的目录版本快照已实现，最终 Debug 360/360、专项 13/13、13 项各 10 次和纯生产/架构通过，见 [C2b3 交付](../阶段交付/2026-09-04-ST1C2b3-目录版本与失效.md)。真实同库三进程 epoch 更换已验证；仅对同一 Runtime/作用域的完整令牌比较失效，不赋予执行或跨目录原子写权限。后续进入 C2c，C3/C4/C6–C8 和最终 ST1D 仍未签核。

C2c1 本地检查点通过：停止确认/失败重试已修复，Debug 368/368、专项 8/8、停止相关 10 项各 10 次和纯生产/架构通过，见 [C2c1 交付](../阶段交付/2026-09-04-ST1C2c1-停止确认与失败重试.md)。C2c2 仍须完成生命周期线程、非协作任务析构依赖和持久化独占释放验证，见 [C2c 契约](ST1C2c-停止确认与析构依赖.md)。不将局部停止状态修复代签 C2c/C5/ST1D，整体未冻结。

C2c2 后续检查点通过：最终 drain 先于 Runtime/模块/持久化释放，工作线程自等待/自销毁受约束；Debug 373/373、专项 15/15、15 项各 10 次、纯生产/架构和真实进程矩阵定向 ASan 3 轮通过，见 [C2c2 交付](../阶段交付/2026-09-04-ST1C2c2-最终排空与析构依赖.md)。进程门禁曾漏掉销毁后接管步骤，已修复脚本编码并逐轮验证六个子检查；旧进程绿灯不代替最终证据。C2 形成本次大节点，下一步 C3，整体尚未 Frozen。

C3a 本地检查点通过：自动关闭快照键不再拼接 DocumentId、墙钟或进程计数，保持旧格式和不可变冲突校验；Debug 378/378、专项 8/8、新增 5 项各 3 次、纯生产及定向 ASan 三轮进程验证通过，见 [C3a 交付](../阶段交付/2026-09-04-ST1C3a-自动关闭快照身份.md)。显式文件键别名/兼容、排序及历史身份域继续按 C3b/c 完成；另将私有头自动边界扫描缺口登记于 C8，不把 71/139 的既有扫描范围扩大解释。整体未 Frozen。

C3b 本地检查点通过：精确 SnapshotId 映射固定摘要文件名，外层信封核验身份；旧格式大小写归属、双格式拒绝、临时文件碰撞、长路径及真实共享冲突回归已完成。Debug 391/391、新增 13 项各 30 次、定向 ASan 34 项各 3 次和纯生产/架构通过，见 [C3b 交付](../阶段交付/2026-09-04-ST1C3b-显式快照文件键与兼容.md)。新格式不支持只回退旧二进制；C6 记录格式/预算，C7 测量编码和核验成本，C5/ST1D 保留支持矩阵边界。下一步 C3c；整体未 Frozen。

C3c1 已增加六项排序/历史身份回归，Debug 与定向 ASan 均各 3 轮、18/18，见 [C3c1 交付](../阶段交付/2026-09-04-ST1C3c1-排序与历史身份回归.md)。证据覆盖回拨时水位优先、同水位确定排序、损坏记录拒绝回退，以及 Journal/Snapshot/混合与 Detached/Removed 来源的身份保护。本次没有生产修改、不宣称全量 397/397；发现 latestSnapshot 与 recover 的 Journal anchor 认证范围不同，下一步 C3c2 必须实测负向材料并收口，C3 不签核，整体仍未 Frozen。

C3c2 正确性检查点通过：已复现并修复公开快照读取遗漏 Journal 锚点、对象内容矛盾及跨水位归属的问题，两入口共用校验；Debug 401/401、专项 10 项各 3 次、统一 C3 ASan 48/48 及另 10 项各 3 次、纯生产/架构通过，见 [C3c2 交付](../阶段交付/2026-09-04-ST1C3c2-统一快照锚点认证.md)。C3a/b/c 形成大节点，下一步 C4；未入 Journal 历史基线的导入权限与读取成本仍由 C4/C6/C7 收口，不能将部分认证增强代签全部 Kernel Frozen。

C4a 本地检查点通过：Benchmark 不再使用公开 attach 注入零修订对象，改由受治理种子命令提交并形成 Journal/History barrier；Debug 与 ASan 各 7 项 × 3 次、21/21，42 份报告核对，见 [C4a 交付](../阶段交付/2026-09-04-ST1C4a-基准种子事务化.md)。本次不修改生产 API，没有重跑全量 401 项；公开入口仍未封闭，下一步 C4b/c，不能代签整个 C4 或 Frozen。

C4b 本地检查点通过：公开 attach 已移除，内部恢复安装要求既有同归属 Detached 身份，剩余夹具迁移并加入真实 Host 编译拒绝门禁；Debug 402/402、定向 12/12、ASan 23 项各 3 次和纯生产/架构通过，见 [C4b 交付](../阶段交付/2026-09-04-ST1C4b-封闭公开镜像装载.md)。这是源码访问边界与迁移检查点，不代签恢复安装失败的完整状态保证；下一步 C4c，整体仍未 Frozen。

C4c 后续检查点通过：新增五项覆盖 22 个导入/安装故障和重入场景，最终 Debug 407/407、统一 C4 定向 ASan 29 项各 3 次（87/87）、纯生产 Release、真实 Host 编译及架构通过，见 [C4c 交付](../阶段交付/2026-09-04-ST1C4c-恢复失败准入与事务导入矩阵.md)。C4a/b/c 形成大节点；已实测无可执行半状态，但恢复失败可保留只读诊断镜像，Interrupted Opening 不自动修复，Failed Host 普通 shutdown 被拒绝而析构后新 Host 可接管。下一步 C5 支持矩阵、C6–C8/ST1D，整体未 Frozen。

C5c 本地检查点通过：建立实际允许/本机验证/未验证/不支持的存储矩阵，新增六种数据库/快照/资产成套恢复场景；Debug 定向 35/35、新增另三轮、ASan 定向 35 项各 3 次（105/105）、纯生产/架构通过，见 [C5c 交付](../阶段交付/2026-09-04-ST1C5c-存储支持与离线恢复验证.md)。完整归档与合法文件超集可用，遗漏快照、当前或历史资产均拒绝且保留材料；这不是在线备份或 ReFS/非本地环境证明。下一步 C6，C5/ST1D 最终支持范围和整体 Frozen 仍未签核。

C6a 本地检查点通过：五个文件路径选项在 OS/库调用前拒绝内嵌 NUL，两类日志输出先完成参数预检，并用一致的 spdlog 宽字符配置保留 Windows Unicode 文件名及轮转。四项新回归在原实现 0/4，修复后全量 Debug 412/412、路径/日志 6 项各 3 次、定向 ASan 54 项各 3 次（162/162）、纯生产/架构通过，见 [C6a 交付](../阶段交付/2026-09-04-ST1C6a-文件路径精确性与Unicode日志.md)。[C6 契约](ST1C6-公共契约与输入预算.md) 保留 API/DTO/格式逐项审查、编码/路径别名补查、统一预算与有界终态保留；私有头扫描缺口仍由 C8 补齐。本节点不关闭整个 C6 或整体 Frozen。

C6b1 本地检查点通过：五类文件路径统一拒绝非法 UTF-16，SQLite 不再因该输入逸出 Result，Snapshot 错误诊断不转换不可编码路径，Asset cause 保持。相关 Debug 57/57、ASan 57 项各三次（171/171）、路径/日志 9 项各三次及纯生产/边界通过；见 [交付](../阶段交付/2026-09-04-ST1C6b1-路径编码准入与契约登记.md)。新增三项回归包含 52 个内部场景；[逐项账本](ST1C6b-公共契约逐项审计.md) 登记 8 个 Adapter、13 个配置字段、6 个 platform 端口。日志物理别名、剩余 API/DTO/错误/持久版本及 C6c/d–ST1D 仍待完成，不宣称全量 415 项或完整契约已通过。

C6b2 本地检查点通过：新增 11 项/36 个内部场景，静态预检日志 base/轮转名称及现有物理身份，保留合法大小写敏感和不同尾随字符目录；最终 Debug 426/426、定向 60/60、ASan 204/204、探针 3/3 和纯生产/边界通过，见 [交付](../阶段交付/2026-09-04-ST1C6b2-日志身份与轮转文件准入.md)。开发期夹具错误、真实路径缺陷、中止轮次及 425/426 架构失败均保留；修复保持 Windows/spdlog 分层，未放宽门禁。Foundation/Observability/Messaging 的后续风险只登记、不借日志测试签核；完整 C6–C8/ST1D 与整体 Frozen 仍未完成。

C6b3 本地检查点通过：Schema 工厂先拒绝全部未定义 rootKind，后端不再把未知类型弱化为 Any；合法八种类型及显式 Any 约束保持。真实红灯 0/2，新增四项；Debug/Release 选集各 147/147、ASan 同选集三次 441/441、探针另 3/3 和纯生产/架构通过，见 [交付](../阶段交付/2026-09-05-ST1C6b3-Schema根类型准入.md)。新增 Foundation.SchemaKindInvalid，不改公共头或持久版本。该证据不覆盖 Schema 全部输入预算、消息缺口或普通全集；下一步消息准入与身份/版本语义，C6–C8/ST1D 和整体 Frozen 仍未完成。

C6b4 本地检查点通过：五项真实红灯 0/5，修复未知 delivery/filter 枚举、Notification 跨版本合并和取消后订阅 ID 复用错投。新增八项，含真实双线程；专项 12 项各十次、扩大 Debug/Release 各 161/161、ASan 同选集各三次 483/483、探针另 3/3 和生产/架构通过，见 [交付](../阶段交付/2026-09-05-ST1C6b4-消息准入与订阅身份.md)。公共头及持久格式不变；cancel 不等于在途回调排空，捕获资源析构/复制异常与总队列预算仍须后续核验，C6–C8/ST1D 和整体 Frozen 未完成。

C6b5 本地检查点通过：七项真实红灯 0/7，修复 cancel/析构/重复拒绝的锁内资源释放、锁内用户复制及复制异常中断其他订阅；新增 11 项，含真实双线程资源延寿。最终消息 23 项各十次、Debug 全集 449/449、Release 选集 172/172、ASan 同选集各三次 516/516、探针另 3/3 和生产/源边界通过，见 [交付](../阶段交付/2026-09-05-ST1C6b5-消息回调资源与异常边界.md)。不承诺取消即 join 或全局 OOM 无损；下一步观察服务及其余 C6–C8/ST1D，尚未 Frozen。

C6b6 本地检查点通过：四项真实红灯 0/4，修复非法 Trace 终态、指标有限输入累计溢出、诊断未知状态及重复注册资源析构重入；新增九项，含生产私有算术计数极限和真实双线程结束。观察专项 13 项各十次，Debug/Release 选集各 181/181、ASan 同选集三次 543/543、探针另 3/3 和生产/源边界通过，见 [交付](../阶段交付/2026-09-05-ST1C6b6-观察状态与聚合准入.md)。uint64 极限不是海量公共调用，私有头扫描仍归 C8；下一步独立日志出口与等级，整体尚未 Frozen。

C6b7 本地检查点通过：六项真实红灯 0/6，修复独立日志出口的 DTO/时间差/异常，以及 Spdlog 等级/纪元前时间准入；新增九项。专项 12 项各十次、Debug/Release 选集各 212/212、ASan 同选集三次 636/636、探针另 3/3 和生产/源边界通过，见 [交付](../阶段交付/2026-09-05-ST1C6b7-日志出口与等级准入.md)。含日志路径及两个真实 Headless JSONL 往返，不代签全部日期、普通全集或全局预算；下一步观察生命周期/资源失败/时间顺序及其余 C6–C8/ST1D。

C6b8 本地检查点通过：Release 红灯在 14 个分配点中的后 6 点复现 `startSpan` 已发布活动身份却无法交付句柄；修复为先分配未激活句柄再发布并激活。Debug/Release 选集各 215/215，ASan 同选集三次 645 次执行，独立 Release/ASan 探针各穷举 14 点；生产构建及 71 公共头/141 生产源边界通过，见 [交付](../阶段交付/2026-09-05-ST1C6b8-Trace准入原子性与身份保留.md)。未运行普通 470 项全集；下一步完成阶段资源失败与 Diagnostics latest 时间顺序。

C6b9 本地检查点：真实 Release 红灯确认 `end noexcept` 和 abandoned 析构在分配异常后会吞错并遗留活动身份；修复为句柄直接持有 Core，在正常 completion 失败后执行无分配、幂等的活动删除。焦点探针覆盖 start 14 点、合法完成 11 点、非法完成 31 点、abandoned 28 点；完成路径每点同时覆盖一次性与持续失败。最终多配置成绩见 [交付](../阶段交付/2026-09-05-ST1C6b9-Trace完成资源失败原子性.md)；下一步 Diagnostics latest 并发顺序。

C6b10 本地检查点：受控双线程红灯确认同一 Diagnostics check 可同时进入、最大并发为 2，且较早调用会在较晚调用后覆盖 latest。修复为每注册项独立串行至本地 latest/exporter 快照发布，保留不同 ID 并行和锁外 exporter；同 ID 递归转换为 `Diagnostics.CheckReentered`，见 [交付](../阶段交付/2026-09-05-ST1C6b10-Diagnostics并发与latest顺序.md)。下一步观察 exporter 失败记录资源边界。

C6b11 本地检查点：真实 Release 红灯确认 exporter 返回 Error/抛异常后，其失败记录 OOM 会跳过后续 exporter，并使 Metrics/Diagnostics 异常逸出。修复为三服务逐 exporter 独立、尽力写有界失败窗口；隔离进程新增 3×2×2=12 条一次性/持续分配失败路径，见 [交付](../阶段交付/2026-09-05-ST1C6b11-观察出口失败记录资源隔离.md)。完整 exporter 快照复制资源语义已由 C6b12 接续。

C6b12 本地检查点：真实 Release 红灯确认 Trace 完成记录和 Diagnostics latest 已发布后，完整 exporter 向量复制 OOM 会跳过出口，并使 Diagnostics 异常逸出；Metrics 的旧复制点位于聚合提交前，保持无半发布。修复为三服务发布时冻结 exporter 数量、逐项锁内取得共享所有者并锁外调用，见 [交付](../阶段交付/2026-09-05-ST1C6b12-观察出口快照无分配发布.md)。后续类型审计由 C6b13 接续，统一预算仍归 C6c/C7。

C6b13 本地检查点：源码复核与真实 Release 负例确认 Workflow/Script 注册器会接纳四类未知定义枚举，旧实现拒绝断言 0/4。修复为注册前白名单验证 Workflow Step/Predicate 与 Script Node/Predicate/WaitTarget，五分支 10 条断言通过，见 [交付](../阶段交付/2026-09-05-ST1C6b13-编排定义枚举准入.md)。下一步继续 Host/执行/状态/持久族类型与格式。

C6b14 本地检查点：真实 Release 红灯确认资源配置、外部 Effect descriptor 与 Task 仲裁会接受未知枚举，重复 Shared units 还可回绕后实际执行。修复为配置/注册/仲裁三层具名准入及加法前溢出检查；最终 Debug/Release 选集各 219/219、ASan 三轮 657 次、纯生产 31 项及 71/141 边界通过，见 [交付](../阶段交付/2026-09-05-ST1C6b14-资源声明枚举与算术准入.md)。下一步继续 Host、其余执行、状态与持久族类型/格式。

C6b15 本地检查点：真实 Release 红灯确认 Command/Query 未知 VersionResolution 被误归 UnsupportedVersion，且两条失败 Trace 均写成 compatible；旧实现六条目标断言失败。修复为 Registry 查找前闭集拒绝并统一观察名称；最终 Debug/Release 选集各 221/221、ASan 三轮 663 次、纯生产 31 项及 71/141 边界通过，见 [交付](../阶段交付/2026-09-05-ST1C6b15-版本解析策略枚举准入.md)。下一步继续 Host、状态与持久族类型/格式。

C6b16 本地检查点：真实 Release 红灯确认 Snapshot Persistence 忽略成功 disposition，未知值和不匹配 AlreadyPresent 均会建立可信索引。修复为索引前闭集验证及 AlreadyPresent 精确读回；最终 Debug/Release 选集各 222/222、ASan 三轮 666 次、纯生产 31 项及 71/141 边界通过，见 [交付](../阶段交付/2026-09-05-ST1C6b16-快照存储写入证明准入.md)。下一步继续 Host、其余状态与持久族类型/格式。

C6b17 本地检查点：真实 Release 红灯确认 Journal 变更/历史、Task 资源/状态/Error 与 Workflow 定义/状态/Error 共 15 个非法分支可被持久化或错误归类。修复为序列化和事务前闭集/形状验证，拒绝不留记录；Release 全集 480/480、Debug 选集 225/225、ASan 三轮 675 次、纯生产 31 项及 71/141 边界通过，见 [交付](../阶段交付/2026-09-05-ST1C6b17-持久DTO写前准入.md)。下一步继续 Host、State、Task Error cause 与其他持久类型/格式。

C6b18 本地检查点：Task terminal 新写入升级为 v2，完整保存含根最多 32 层 Error cause；任意层未知枚举、环和第 33 层均在写前拒绝，摘要有效但 cause 畸形/超深的 v2 读取失败关闭。历史 v1 继续读取，无 cause 的精确等价终态可在升级后幂等重放；最终 Release 全集 484/484、Debug/Release 扩大选集各 225+4、ASan 三轮 687 次通过，见 [交付](../阶段交付/2026-09-05-ST1C6b18-Task错误cause版本化持久化.md)。下一步继续 Host、State 与其他持久类型/格式；C6 仍未闭合。

C6b19 本地检查点：Host/Module、Project/Document Runtime 与 PersistenceOwnership 状态被确认为只读观察结果，不增加任意整数写入口；RevisionScope 和 ObjectPersistencePolicy 继续通过真实闭集入口拒绝未知值，`RevisionSet::at()` 明确为已验证 scope 的 `noexcept` 直接访问器。Project/Document persistence state 才是公开写入材料；Project 已有负例，本节点新增 Document 6/255 未定义值拒绝并用独立 SQLite 连接确认行级零变更。两个 lifecycle v1、SQLite schema、公共头和生产实现均未改变；Release 全集 486/486，定向 Debug/Release 各 11/11，ASan 三轮 33 次通过，见 [交付](../阶段交付/2026-09-05-ST1C6b19-Host与State状态边界.md)。该检查点之后的 Persistence 剩余格式现由 C6b20 承接完成；C6 尚未闭合。

C6b20 本地检查点：Release 红灯确认 `claimCommand()` 会把首次占位后端返回的 0/2 行成功计数误报为 Acquired；修复为仅恰好一行成功，否则回滚并返回 `Persistence.IdempotencyClaimWriteCountInvalid`，同键可干净重试。四项新增回归共 112 条断言，覆盖非法 ReplayPolicy/DiagnosticStatus/纪元前时间零写入、摘要有效的不支持格式拒绝及 command outcome v1/v2 精确兼容；既有 ExternalEffect 恢复用例补未知策略/状态读取拒绝。最终 Debug/Release 定向各 10/10、ASan 三轮 30 次、Release 全集 490/490（1084.04 秒）、纯生产与 71/141 边界通过，见 [交付](../阶段交付/2026-09-05-ST1C6b20-持久执行与诊断格式边界.md)。下一步做 71 头与 C6b1–b20 最终对账；C6 尚未闭合。

C6b21 本地检查点：以 `9ea3429` 为实现基线复核当前 71 个公共头，逐文件摘要与 C6a 清单 71/71 一致；逐头登记公开面、权限阶段及 C6c/d/C7 剩余项，补齐此前漏记的 5 个真实错误码后，C6b 生产增量的 51 个新增/变更码全部可追溯，并形成 SQLite schema 9 及各逻辑/外层格式的读写兼容矩阵。该节点仅文档对账，复用紧邻 C6b20 的代码门禁并另跑摘要、错误码、文档链接和差异格式检查，见 [交付](../阶段交付/2026-09-05-ST1C6b21-71公共头与兼容清单最终对账.md)。C6b 已签核；下一步 C6c，C6 仍未闭合。

C6c1 本地检查点：真实 Release 红灯确认 JSON/TOML 序列化和 Schema constraints 会接纳第 65 层材料；修复后共享 `ValueBudget` 统一限制深度 64、节点 100000、文本 16 MiB、编码输入输出 64 MiB，并在 Schema、JSON/TOML 输入/输出与第三方 backend 前失败关闭。Debug/Release 焦点各 8/8、ASan 三轮 24 次、Release 全量 498/498（1081.59 秒）、纯生产及 71/141 边界通过，见 [交付](../阶段交付/2026-09-05-ST1C6c1-共享Value与序列化预算.md)。当前仅 `foundation/value.hpp` 有意改变摘要且 Value 布局未改；下一步 C6c2，C6c 与 C6 均未闭合。

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
