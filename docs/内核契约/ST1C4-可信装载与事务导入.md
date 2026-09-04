# ST1C4 可信装载与事务导入

## 范围与执行顺序

承接 [补充执行计划](ST1C-补充审计与剩余执行计划.md) 的 C4。起点是已推送远端的 C3 正确性节点 `02fff14`。本节点只处理内核公开入口、恢复安装、事务导入和测试依赖；不实现 STEP/工程包解析器、领域导入模块或产品命令。

当前 C4a/b/c 正确性检查点通过：最终 Debug 407/407、统一 C4 定向 ASan 29 项各 3 次（87/87）、纯生产/真实 Host 编译/架构门禁通过，证据见 [C4c 交付](../阶段交付/2026-09-04-ST1C4c-恢复失败准入与事务导入矩阵.md)。形成远端大节点；下表 C4a/b 的数字保留各自历史范围，不代替最终统一结果。C5 支持矩阵、C6–C8 和 ST1D 仍待闭合，整体未 Frozen。

| 子节点 | 内容与验收 | 状态 |
| --- | --- | --- |
| C4a | Benchmark 去除公开 attach 种子，改走受治理 Command/Transaction；种子有修订与 History barrier，SQLite 有真实 Journal；恢复与后续 Undo/Redo 断言同步调整 | 本地检查点通过：Debug/ASan 各 7 项 × 3 次，21/21，42 份报告核对；见 [C4a 交付](../阶段交付/2026-09-04-ST1C4a-基准种子事务化.md) |
| C4b | 删除公开镜像 attach，内部恢复安装私有化；迁移全部调用夹具，不提供测试后门；真正的 Host 编译拒绝探针和正常入口编译对照 | 本地检查点通过：Debug 402/402、定向 12/12、ASan 23 项各 3 次及纯生产/架构通过，见 [C4b 交付](../阶段交付/2026-09-04-ST1C4b-封闭公开镜像装载.md)。下一步 C4c，不代签整个 C4 |
| C4c | 可信导入负向矩阵、运行期 open 和 bootstrap 安装失败检查；无可执行半状态，拒绝后材料可诊断、重试或新 Host 恢复符合契约；统一 C4 回归 | 新增 5 项、22 场景；Debug 407/407、统一 ASan 29 项各 3 次及纯生产/编译/架构通过，见 [C4c 交付](../阶段交付/2026-09-04-ST1C4c-恢复失败准入与事务导入矩阵.md) |

各子节点独立本地提交；C4b/c 全部达到验收后才汇总并推送 C4 大节点。C4a 没有封闭生产 API，不能标记 C4 通过；不以历史 Debug 401/401 代替新代码全集。最后三配置签核仍归 ST1D。

## 已核实的旁路及调用依赖

起点的 `DocumentRuntime::attach(DocumentImage)` 是公开方法，接受调用者指定的对象、归属和修订；它经过对象类型/资产检查，但直接 restoreDocuments，不产生正常业务提交的 Journal/History。类型合法不等于导入已获授权。该方法同时被 `openImpl` 用来安装 recover 认证后的镜像，必须区分两种来源，不能一并删除合法恢复能力。

除 openImpl 外，调用依赖分布在 Benchmark、Project/Document 生命周期、对象类型、资产、持久化和停止后的入口拒绝测试。迁移规则如下：

- Benchmark 的对象通过测试模块注册的普通 DocumentWrite 命令创建，不新增生产种子 API。Journal 适配器基准仍可直接测试 PersistenceService，但必须追加网关实际产生的种子提交后再 capture，不能造零修订历史基线。
- 生命周期测试用 create 创建空身份，用配置真实持久化后的 open 重开；无持久化的 Detached 文档按既有契约返回 OpenRequiresPersistence，不为测试保留镜像复活后门。
- 原 attach 的类型/资产负例迁移后必须明确各自证明事务导入还是持久恢复，不能用删除旧断言或组件验证冒充 Host 恢复准入。
- 停止后的负例保留仍存在的公开入口；被删除的入口改由真正的编译拒绝证明。现有文本架构扫描不是负向编译测试。

## 公开与内部边界

Host 仍可执行受治理的生命周期操作和正常 Command/Query；空身份创建不接受外来业务对象或任意 Revision。业务数据导入必须使用已注册、具有 Capability/Scope/Schema 的命令，在 ApplicationTransaction 中形成候选，由统一对象类型、引用、资产、冲突及持久提交链决定接纳；导入方不能指定最终修订、调用 restoreDocuments 或自行安装 History。

本节点用内核测试模块表达导入，不增加通用“任意镜像替换”产品命令。新建空文档和导入命令是两个显式操作；导入失败允许保留已明确创建的空文档，但对象、修订、Journal/History 和成功事件必须不产生部分业务提交。需要单一“创建并导入”的上层体验时不能据此虚构跨操作原子性；本阶段不扩展该组合产品功能。大输入和长计算的预算/Task 化由 C6/C7 承接。

内部安装只能服务已认证的持久恢复，核验精确身份和归属、合法目录状态，不允许重新绑定 Removed/Detached 历史或从调用者镜像建立新身份。配置期 createDocument 是空身份声明，不是授权替换旧历史；C3 的 Journal/Snapshot/混合、同归属/异归属和墓碑矩阵继续保留。旧 Snapshot-only 材料的读取兼容不授予 Host 新的任意导入权限。

## 失败与回归要求

起点 attach 的 Open 持久写失败发生在 DocumentStore 安装之后，虽然目录置 Failed，仍需实测实际执行链是否拒绝，以及只读诊断、重试、停止和重启行为。不能未经验证就将“返回 Error”写成完全回滚，也不能宣称失败时没有任何 SQL 写入。bootstrap 同样核对失败 Host 的准入、安装状态与持久材料影响。

1. 编译门禁：普通 Host create/open 编译成功；attach 及替代内部安装、DocumentStore 恢复写入口不可调用。负例必须因预期的访问/不存在原因失败，不把缺头、链接失败或工具链不可用当作通过。
2. 事务导入：合法对象及显式旧 Schema 保留；未知类型/版本、无效对象、重复 ID、引用和资产不合法、无权限/错误 Scope 均拒绝。覆盖多对象候选中后项失败，防止仅验证第一项或错误后仍提交前缀。
3. 持久提交失败：检查导入前后对象、全部修订、Journal、History、事件和活动租约；保留故障材料，解除注入后重试和重启验证，不自动重放 Handler。
4. 恢复安装失败：覆盖类型/资产准入、Opening/Open 持久写失败、项目关闭/错误归属/历史墓碑、并发或重入状态竞争；失败不得被 Command/Query/Task/Workflow/Script 当作 Open 状态执行。合法重开不重置项目修订或文档 History。
5. 汇总：针对已发现缺陷的修复先在原实现取得红灯（C4b 已有公开 attach 实际编译成功的旁路红灯）；对既有正确行为增加回归不要求制造生产缺陷，夹具错误不能充当生产红灯。最终相关组件和全量 Debug、定向 ASan、生产独立构建、真实 Host 编译探针及架构门禁通过后签核 C4。构建/测试结果不外推同进程恶意代码隔离、任意外部材料篡改或物理断电保证。

## C4c 已核实的失败状态契约

| 失败位置 | 当前 Host | 持久事实与后续 |
| --- | --- | --- |
| 导入事务 Begin/Journal Execute/Commit 失败或异常 | 候选对象和暂存事件不发布；原对象、全部修订、Journal、History 与目录不变，活动租约释放，原文档仍可查询 | 测试中不带幂等键的请求允许调用方显式重试；成功后新 Host 恢复并实际 Undo/Redo。不得套用为幂等失败缓存的自动重放规则 |
| 恢复安装的 Opening 写失败 | 文档 Failed，尚未安装对象，全部执行入口拒绝 | 原始目录仍为 Detached；销毁后新 Host 保持 Detached，显式 open 可恢复，不自动执行 Handler |
| 已安装镜像后的 Open 写失败 | 文档 Failed；只读 DocumentStore 可保留完整认证镜像，但 DocumentRuntime.snapshot、Command/Query/Task/Workflow/Script/Undo 拒绝 | 原始目录为 Opening；连续两个新 Host 都解释为 Failed + RecoveryInterruptedTransition，保留原始行且拒绝再次 open，不自动改成 Detached/Open |
| bootstrap 已安装 Store/History，后续配置目录同步失败 | Kernel Failed，可能留有只读诊断状态，运行入口未开放；再次 bootstrap 与普通 shutdown 均拒绝，shutdown 错误为 Kernel.AppKernelCannotStop | 必须销毁 Failed Host，由析构屏障释放依赖/独占权；新 Host 可接管未被改写的既有材料，不带入未提交的配置文档 |
| 对象验证回调中移除身份或另一合法 open 完成 | 外层安装分别因 LifecycleNotFound/LifecycleConflict 拒绝，不覆盖赢家、不追加生命周期写入；项目关闭与 Kernel shutdown 在途拒绝且不开始 SQL 事务 | Removed 墓碑不得复活；合法内层 open 的完整 Open 状态可用且重启保留。不能将外层失败错误地理解成整个文档必须不可用 |

上述是故障可诊断和准入隔离，不是“所有恢复失败都完全回滚”。Interrupted Opening 没有新建通用修复入口；需保留原始数据库/文件并按 C5/C6 的受支持恢复流程处理，不能直接改 SQL 状态来绕过认证。这里的“新 Host”是销毁并新建实例，不冒充独立进程或硬件断电证明。

## 后续承接

C4b 当前实现：原公开 attach 已移除，私有 attachRecovered 仅由 openImpl 调用，安装前要求既有同归属 Detached 目录项，不再接受未登记身份。Host 编译门禁对真实头文件编译正常 create/open/snapshot，再分别编译 attach、attachRecovered、adoptRecovered 和独立 DocumentStore.restoreDocuments 的调用；四个负例只接受对应成员的 MSVC C2039/C2248，缺头、语法、链接或环境错误不是成功。独立静态编译探针只证明当前 MSVC 工具链的源码访问边界，不声称链接、ABI 或 ASan 运行期证明。

原类型 attach 测试改为普通受治理导入命令：两个对象中后项未知类型/版本、无效值或重复身份时回滚整批；已支持的旧 Schema 不隐式升级，缺权限/文档 Scope 时 Handler 不执行。资产/Transient 负例也改为命令导入已显式创建的空文档，失败保留空文档但不提交业务状态。原内存生命周期测试确认 OpenRequiresPersistence，项目成员测试使用真实持久化 open；停止/关闭后不再测试已经删除的方法，改由编译门禁覆盖该入口，剩余执行拒绝断言保留。上述不代替 C4c 的完整恢复准入、错误码和失败状态矩阵。

C6 登记 public attach 移除造成的源码兼容变更及导入/恢复权限区别，不承诺跨工具链 C++ ABI。C7 按新种子契约重建基准：种子现在也是实际事务，改变启动内存、Journal 大小、锚点认证和恢复成本；旧零修订种子与新结果不得直接比较为性能回归。C8 继续补私有头扫描缺口，不能以本次源码人工审计代替自动覆盖。
