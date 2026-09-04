# ST1C5 持久化 Host 独占与耐久准入

## 状态与完整目标

承接 C1a `9da759e`。C5 的完整目标是：任何 schema 初始化、abandoned claim 恢复、任务/副作用中断归类之前，必须取得数据库单活动 Host 所有权；第二 Host 不能改写第一 Host 的材料。正常运行和失败隔离期间保留所有权，不能把一次 SQL 事务锁误当作 Host 生命周期锁。

最初分 C5a/b 两步实施：C5a 交付必需端口与 SQLite/Windows 适配器、组件和跨进程证据；C5b 接入 PersistenceService/AppKernel 初始化链、只读诊断及活动 claim 不受第二 Host 影响的回归。后续 [C5c 支持矩阵与离线恢复](ST1C5c-存储支持矩阵与离线恢复.md) 已形成本机本地检查点（Debug 35/35、定向 ASan 35 项各 3 次及纯生产/架构通过），区分真实验证与未验证环境，下一步 C6；最终支持范围与三配置仍由 ST1D 签核。**C5a 单独通过不等于 AppKernel 已强制独占，也不关闭 C5 审计项。**

## C5a 端口与适配器

- `IPersistenceBackend::acquireHostSession()` 是纯虚端口，没有默认成功实现；返回 `PersistenceSessionInfo`，包含 backend、persistent 和实际 configuration。故障/崩溃代理必须转发，合成且无所有权能力的后端明确失败。
- SQLite 从官方 `SQLITE_FCNTL_WIN32_GET_HANDLE` 获得自己实际使用的句柄，不再次按路径打开文件，也不使用可能被绕过的路径字符串或旁路锁文件。句柄解析出的存储只接纳本地固定 NTFS/ReFS 卷；其他卷或无法验证的 VFS 拒绝。
- 在同一文件句柄的 `0x7ffffffffffffffe` 处申请一字节排他、立即失败的 `LockFileEx` 锁。该位置远超当前锁定 SQLite 的最大数据库范围与内部锁区，不写入或增大数据库。同文件、大小写、点路径和硬链接别名由实际文件锁仲裁。
- 所有权留至后端实际销毁和 SQLite 关闭句柄，无公开提前 release。commit、rollback 和应用 shutdown 意图都不是释放点。后端可在不同线程销毁，不依赖线程所有者互斥量的递归/释放语义。
- 文件会话先取得锁，再显式设置 `journal_mode=DELETE`、`synchronous=EXTRA(3)`、`foreign_keys=ON`，随后读回核验；重复准入及已准入连接的 beginTransaction 再读回，发现漂移拒绝，不自动覆盖成“健康”。失败也不提前释放已取得的文件锁。
- `:memory:` 是私有连接会话，标记 `persistent=false` 与 `ownership=private-memory`；其 `journal_mode=memory` 不宣称文件耐久性。未申请 Host 会话的独立低层 SQL Adapter 仍可做测试/诊断；C5b 必须让内核运行链无条件申请会话，不能依赖调用者自觉。

机制依据：[SQLite 原生文件句柄端口](https://www.sqlite.org/c3ref/c_fcntl_begin_atomic_write.html)、[Windows 文件区间锁](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex)、[SQLite synchronous 策略](https://www.sqlite.org/pragma.html#pragma_synchronous)。操作系统关闭句柄/进程退出会释放锁，但不承诺释放零延迟，也不把这些 API 语义或 EXTRA 配置当作设备物理断电认证。

## C5a 证据

当前最终 Debug 构建成功，专项 21/21（2.89 秒）；新增 7 个组件用例及 1 个进程用例。覆盖四种文件别名、20 轮双线程同时准入、跨线程销毁、commit/rollback/VACUUM 后仍独占、不同数据库互不阻塞、三类配置漂移、事务内准入拒绝、私有内存标识、故障代理错误/异常，以及正常退出与强制中断后新进程接管。

进程测试是仅测试构建启用的探针，不是产品 CLI。驱动在隐藏进程中执行，握手/等待有界，异常清理只终止自己创建的子进程。测试沿用 `RunVerifiedProgram.cmake`，同时核验退出状态与成功标记，不用 `PASS_REGULAR_EXPRESSION` 掩盖非零退出。数据库与各轮证据目录保留。

开发期先遇到 Catch2 的 string_view 断言链接差异，改用既有 std::string 断言风格；Windows PowerShell 5 驱动的 UTF-8 文件识别与输入 BOM 问题经字节诊断后修正。相关失败日志保留，但不当作纯应用缺陷证明。

`build/st1c5a-policy-red-verified-tests.log` 才是排除驱动问题后的缓存缺陷证据：6 项中仅配置漂移用例失败，11 条断言中 2 条失败，重复准入及 beginTransaction 错误地成功。修复后扩展为 synchronous、foreign_keys、journal_mode 三种漂移，专项全部通过。

最终源码新增 8 项各重复 10 次，共 80 次（24.08 秒），包含 200 轮双线程争抢与 60 个子进程。纯生产 Release 成功；31 工程、无测试/探针/Benchmark/ASan 工程、无插桩、无 CTest 文件；70 公共头和 136 生产源架构检查通过。完整日志及摘要见 [C5a 交付记录](../阶段交付/2026-09-04-ST1C5a-持久化会话端口与独占基础.md)。当前注册清单为 294 项，仅表示清点数量；尚未执行本节点完整 Debug/Release/ASan 全集，不把 C1a 的 286/286 延伸到本节点。

## C5b 强制集成门禁

1. `PersistenceService::initialize` 先申请会话，成功后才允许任何 schema/恢复 SQL；返回错误、抛异常、存储不支持、策略不符均不得进入初始化。只读诊断展示实际策略和持有状态，不暴露释放或绕过开关；Benchmark 的 SQLite 配置元数据改为准入后采集，不能继续记录初始化前的默认值。
2. 双 AppKernel、双持久服务与跨进程 Host：第一实例活动 claim 仍 pending，第二实例必须拒绝且不改为 abandoned/interrupted；第一实例继续正常完成。不能只测试两个 Adapter 的返回值。
3. 后端故障代理转发新契约，覆盖返回错误与异常。回滚隔离后旧连接仍持有所有权，直到实例与实际后台访问者安全退出；与 C2c 的 drain/超时生命周期一起验收。
4. 既有测试中“活跃 Kernel 旁边另开 PersistenceService 注入数据”的路径须改为明确测试材料注入或适当生命周期分段，仍验证原始错误原因；禁止增加 skipOwnership/testMode 绕过生产门禁，也不能删除原故障断言来换取全绿。
5. 正常释放、跨线程释放、进程中断接管和故障接管均核验；提交前运行受影响全集，最终三配置/项目 crash/新基线仍归 ST1D。C5b 后才进入 C1b 写入端项目/文档修订一致性检查。

当前支持边界为本机受信任 Kernel Host。此协议不阻止不调用端口的独立原始 SQL 工具或同进程恶意代码篡改数据库，也不提供多写者支持。NTFS 本机用例已覆盖；ReFS 分支和非本地卷拒绝的真实环境验证尚未执行，必须在支持矩阵中如实保留。

## C5b 实施记录（本地检查点通过）

已在初始化事务之前强制调用会话准入，覆盖独立 PersistenceService 和 AppKernel 启动链。旧代码红灯证据 `build/st1c5b-red-tests.log`：新增 3 项全部失败，第二 Kernel 能启动并将第一 Kernel 的 pending claim 改成 abandoned，导致正在执行的命令提交失败。修复后同一 3 项通过；最终 Debug 299/299、专项 56/56、新增 5 项各 10 次（包含 60 个子进程）、纯生产及架构通过。日志与摘要见 [C5b 交付](../阶段交付/2026-09-04-ST1C5b-初始化独占与活动状态保护.md)。本地检查点不替代 C2c 生命周期、支持矩阵与 ST1D 最终签核。

C2c2 补充了真实析构联动证据：短停止超时和最终 drain 期间，同进程/跨进程第二 Host 均拒绝；任务、观察及持久终态结束后释放 Host，原进程尚存活时另一进程可以接管。Debug 进程矩阵 10 轮及定向 ASan 3 轮均逐份核对六个子检查；见 [C2c2 交付](../阶段交付/2026-09-04-ST1C2c2-最终排空与析构依赖.md)。该证据不扩展 NTFS 本机以外支持矩阵，也不证明硬件断电耐久性。

只读 `sessionStatus()` 区分 NotRequested（尚未申请）、Unconfirmed（尝试后未获成功证明）、Acquired（已经成功准入）。`ready` 单独表示服务可用；隔离后 ready=false 不意味着文件所有权已经释放。`lastAdmission` 是最近一次成功核验的配置副本，不把缓存展示成故障后的实时健康值。重复 initialize 会重新核验策略，但不再次执行 abandoned/task/effect 恢复；策略漂移后隔离，不允许恢复 PRAGMA 后重新启用旧服务。

Benchmark 改为成功准入后采集 journal mode、synchronous、foreign keys、page size 和 cache size。种子准备经正常 close/open 生成快照，Journal 恢复计时开始前销毁前任持久服务；旧性能基线不能代表新耐久策略和初始化流程，正式新基线仍由 C7/ST1D 产生。

既有夹具适配保留故障覆盖：正常快照走生命周期入口；中断 Task/Workflow 材料在前任 Host 销毁后由测试服务顺序写入；不支持对象版本用明确的原始 SQL 破坏注入，不假扮第二个合法 Host。独立 Snapshot 故障矩阵从私有种子 Kernel 取得真实提交，在唯一文件服务中施加故障，并以新 Kernel 重启恢复断言验证结果。没有新增生产 bypass、skipOwnership 或 testMode。
