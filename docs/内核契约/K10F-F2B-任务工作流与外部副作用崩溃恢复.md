# K10F F2B 任务、工作流与外部副作用崩溃恢复

## 当前范围

承接 [F2A](K10F-F2A-命令事务与历史崩溃恢复.md)，本节点新增 9 个三进程崩溃场景和 2 个 Workflow 错误链单元测试，并修复检查点丢失错误 cause 的问题。生产变更仅在内核 Workflow 持久化实现，不增加公共 API、第三方库、业务模块或产品 CLI。

仍未完成 Asset 发布进程中断与 F2 汇总验收；F3 并发压力、F4 性能/内存和 F5 最终门禁继续待办。不能宣布 Kernel Frozen。

## 新增进程场景

每个场景沿用 `crash-seed → crash-recover → crash-audit`，要求指定退出码 86 和准确退出点标记。使用真实 SQLite、文件 Snapshot、Command/Task/Workflow 执行路径；退出时不运行析构或 shutdown。

| 场景 | 崩溃位置 | 新进程恢复与显式操作 |
| --- | --- | --- |
| `task-handler` | 真实 BS 线程池 worker 已进入 handler、取得 Document snapshot 和独占资源、进度达到 0.5 | 持久历史为 Failed，错误为 Task.InterruptedByRestart；无活动任务。重发原 Command/Key 只重放接受回执；waitTask 返回中断终态，不重建线程执行 |
| `workflow-handler` | Workflow 步骤实际进入 DocumentWrite handler，形成候选但未提交 | 恢复为 Waiting、原 attempt=1；显式 advance 执行同 attempt 一次，完成唯一事务 |
| `workflow-committed` | 步骤 Command SQLite commit 成功，但内存安装和步骤完成 checkpoint 尚未发生 | Document/Revision/History 从 Journal 恢复；显式 advance 复用同 attempt 的已提交回执，不重跑 handler |
| `effect-safe` | Publish 测试 handler 已记录外部调用，尚未返回结果 | 启动只标记 Interrupted；显式原 Key 重试才进入 resumed handler，成功后再重放回执 |
| `effect-idempotent` | 同上 | 同上；内核依据已声明的 Idempotent 策略允许显式重试，不替代适配器自身的幂等实现 |
| `effect-reconcile` | 同上 | 恢复为 ReconcileRequired；显式原 Key 请求失败，保留需对账状态，不调用 handler |
| `effect-never` | 同上 | 恢复为 Indeterminate；显式原 Key 请求失败，保留结果不确定状态，不调用 handler |
| `workflow-effect-reconcile` | Workflow 的实际步骤进入上述外部 handler 后退出 | 启动不执行；显式 advance 使用原 attempt，被 Effect 持久门禁拒绝；Workflow Failed、Effect 仍 ReconcileRequired |
| `workflow-effect-never` | 同上 | 同上，Effect 仍 Indeterminate，不能用 Workflow 恢复绕过原 attempt 的禁止重放策略 |

Task 场景明确检查 worker 线程不是调用线程，退出点的实际状态为 Running、进度 0.5、活动任务数 1；不是直接插入一条假 Running 记录。恢复后的公开 TaskState 没有新加 Interrupted 枚举，而是沿用 `Failed + Task.InterruptedByRestart`；底层持久状态为 interrupted。

每轮在专用目录保存 `execution-calls.log`，由 handler 写入并检查关闭成功，独立于 Kernel 的 SQLite 事务。恢复启动后必须仍只有一次调用。最终计数：未提交的 Workflow Document handler 为 2 次（一次未提交、一次显式补做）；Safe/Idempotent 为 2 次（原调用和显式重试）；其余场景均为 1 次。第三个审计进程再次启动、读取或重放后不得增加计数。

这只是软件测试调用的外部可观察证据，不是机械设备调用或断电耐久性证明。

## 跨子系统的一致性

- 所有场景保持基线 Document/Revision/Journal/History；只有两个 DocumentWrite Workflow 场景最终增加一次目标对象事务。六类 Revision、对象材料与 History 游标沿用 F2A 的逐项断言。
- Task 接受回执与 TaskId 一致，恢复保留原 sourceRevisions，无活动任务或独占资源残留。
- Workflow 步骤崩溃时的持久 checkpoint 为 Running；启动后的内存为 Waiting、replayCurrentAttempt=true；advance 不增加 attempt。
- 已提交 Workflow 回执不会增加第二条 Journal；终态再次 advance 无操作。
- External Effect 的持久记录与普通 command_idempotency 分离；启动与拒绝重试均不删除或伪造成功记录。
- Never/ReconcileOnly 的 Workflow 错误、步骤状态与 Effect 状态在第三个进程仍然一致；禁止仅凭 handler 次数或只检查一个错误返回宣称恢复正确。

## 实测缺陷与修复

初始 9 场景中，7 个通过，两个 `workflow-effect-*` 场景在第三个审计进程失败。独立调用记录仍为 1，说明没有不安全重放；实际缺陷是 Workflow error 序列化只保存外层 code/category/severity/message/details，遗漏 cause。第二次启动后只能看到 `Effect.DurableClaimFailed`，丢失底层 `Persistence.ExternalEffectIndeterminate` 或 `Persistence.ExternalEffectReconcileRequired`。

初始失败证据保留在 `build/vs2022/tests/crash-contract-runs/workflow-effect-reconcile-7d3efae7abc5c50b` 与 `workflow-effect-never-2b14ee6b952258d1`，包含失败审计日志和仍为一次的调用记录；修复后使用新目录重新验证，没有覆盖原失败材料。

修复 `workflow_persistence.cpp` 的实例、步骤与补偿错误编解码：

- 有 cause 时增加嵌套字段，保留每个节点的 code/category/severity/message/details；没有 cause 的叶节点不添加字段，旧版 wire version 1 叶错误格式保持兼容。
- 写入与读取最多支持含根节点的 32 个错误节点。超深写入通过既有异常边界返回 `Persistence.SaveWorkflowFailed`，原检查点保留；不是静默截断。
- 已存储的畸形或超深 cause 返回 `Persistence.InvalidWorkflowPayload`，不把损坏材料解释为没有原因。
- 根 checkpoint 与独立 step 行的摘要和完整材料一致性检查保持原样，没有放宽校验来让新测试通过。

兼容性结论是新版本可读取旧版无 cause 材料；不据此承诺旧二进制能够读取新增错误链，更不能补回旧文件中从未保存的原因。

新增两个单元测试覆盖 1/32 节点在实例、步骤、补偿错误中的保真、无 cause 旧格式、33 节点写入拒绝与原材料保留、重新计算摘要后的畸形/超深持久材料拒绝。摘要有效仍需验证结构，不能把 Hash 通过等同于 checkpoint 合法。

## 复现与门禁

```powershell
cmake --build --preset vs2022-debug --parallel 1 -- /nodeReuse:false
ctest --preset vs2022-debug -R 'integration.kernel_crash_(task|workflow|effect)|Workflow checkpoint errors|Workflow checkpoint rejects' --output-on-failure
ctest --preset vs2022-debug -R 'integration.kernel_crash_(task|workflow|effect)|Workflow checkpoint errors|Workflow checkpoint rejects' --repeat until-fail:20 --output-on-failure
```

本节点最终门禁：

- Debug/Release 警告错误构建通过；Debug 全集 230/230（36.50 秒），Release 全集 230/230（49.47 秒）。
- 9 个新增崩溃场景和 2 个错误链单元测试各连续重复 20 次，共 220 次 CTest 执行通过（239.81 秒）；其中 180 次三进程场景共启动 540 个子进程。无 flaky、超时或部分提交。
- F2A/F2B 的 19 个进程场景及错误链测试同时进入全量回归，原 F1 故障矩阵仍在全量中通过。
- Production-only Release 通过，31 个工程中测试/contract/Catch2 目标为 0，CTest 文件为 0，Catch2 源码目录不存在。
- 架构扫描通过 69 个公共头文件和 133 个生产源文件，既有架构负例随全集通过；Git 差异检查与中文文档链接检查通过。
- 初始缺陷的两个失败目录保留；修复后的再次审计退出码为 0，禁止重放场景的独立调用记录仍为一次。

日志为 `build/k10f-f2b-debug-tests.log`、`build/k10f-f2b-release-tests.log`、`build/k10f-f2b-repeat-tests.log`、`build/k10f-f2b-focused-tests.log` 及对应 `*-build.log`。本节点本地提交；完整 K10F 大节点通过后再推送远端。

源证据：`tests/integration/kernel_crash_contract.cpp`、`tests/unit/runtime/persistence_service_tests.cpp`、`src/runtime/persistence/workflow_persistence.cpp`。进程数据库、Snapshot 和日志仍保留在忽略的构建子目录中。

## 剩余边界

不宣称 Task 线程可恢复，不自动重放外部副作用，不实现自动对账或 Machine-specific safety。外部调用、磁盘断电、控制器/SDK/实机验证不在这些软件门禁之内；Asset 发布的进程中断仍待 F2 后续节点。
