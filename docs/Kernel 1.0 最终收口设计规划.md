# LaserCNC v3.0 Kernel 1.0 最终收口设计规划

## 1. 目标

当前 LaserCNC Application Kernel 已完成 Foundation、Application Composition、Infrastructure Adapter、Document/Revision/Transaction、Command/Query/Event、Task/Scheduler、Observability、Persistence/Recovery、Workflow/Script 等 Phase 1–9，并已经建立较严格的架构边界。

现阶段不再追求“增加更多通用能力”，而是完成业务模块接入前的最后一次内核收口，使其达到：

**安全、稳定、可恢复、可测试、可扩展、不可旁路，并能长期作为 CAD/CAM/Machine/Process/CLI/AI 的唯一 Application Kernel。**

完成本规划后：

> Kernel 进入 `1.0 Frozen` 状态。

后续 CAD、CAM、OCCT、Machine、Collision、Process、Qt、CLI、RPC、AI 等均作为 Kernel 之上的模块开发，原则上不得再因为单个业务需求修改 Kernel 核心语义。

> 2026-09-05 执行更新：本文件保留总体蓝图，当前可执行节点以 [ST1C 补充执行计划](内核契约/ST1C-补充审计与剩余执行计划.md) 为准。C6b20 已修复 Idempotency 首次占位未核验精确影响行数的缺陷，并冻结 ExternalEffect、Diagnostic、Recovery/Session DTO 与 command outcome v1–v3 兼容边界。下一步依次为 71 个公共头及 C6b1–b20 最终对账、C6c 统一预算、C6d 同步/Task/终态保留，再进入 C7/C8 与 ST1D；在这些节点完成前不得把局部持久格式证据写成 Kernel Frozen。

---

# 2. 当前成熟度判断

当前不是需要重构的“内核雏形”，而已经是一套较成熟的 Application Kernel。

综合当前代码，我建议按以下成熟度理解：

| 领域 | 当前成熟度 | 目标 |
|---|---:|---:|
| Foundation / Result / Error / Schema | 95% | Freeze |
| AppKernel / Module 生命周期 | 90% | 补治理后 Freeze |
| Document / Revision / Transaction | 85% | 补生命周期与 History |
| Command / Query Runtime | 85% | 补契约版本与执行范围 |
| Task / Scheduler / Resource | 90% | 补外部访问限制 |
| Event / Observability | 90%+ | 基本 Freeze |
| Persistence / Crash Recovery | 90% | 配合 History/Effect 补强 |
| Workflow / Script | 90% | Freeze |
| 开放式 Command Backend | 80% | 补发现及契约 |
| 外部硬件副作用执行 | 40% | **必须完成** |
| Undo / Redo | 30–40% | **必须完成** |
| 运行期 Project/Document 生命周期 | 50% | **必须完成** |
| Kernel 对未来 OCCT/Data Plane 边界 | 70% | 补契约后 Freeze |

因此：

> **Application Kernel 本身已经约 9/10 成熟；但作为最终 CAM 产品底座，目前约为 7/10。**

差距主要不是缺少框架，而是若干关键契约尚未闭环。

---

# 3. Kernel 1.0 必须坚持的最终原则

业务开发前，将以下原则升级为不可违反的 Kernel Contract。

### 3.1 Command-First

所有用户可触发业务行为必须最终进入：

```text
CommandRuntime
```

包括未来：

```text
GUI
CLI
Script
Workflow
AI
RPC
Machine
Process
```

不得因为 Machine 或 OCCT 性能要求而另建第二套业务入口。

现有架构规则已经明确 GUI、CLI、Script、AI、RPC 应共享 Command/Query/Workflow 入口，这一原则继续保持。

---

### 3.2 Query-Only Read

所有公开状态读取通过 Query。

模块不得获得活动 Document 的可变访问能力。

---

### 3.3 Task-Only Long Computation

所有可能长时间运行的计算：

```text
STEP Import
Meshing
CAM Toolpath
Simulation
Collision
Index Generation
Post Processing
```

都必须通过：

```text
TaskRuntime
→ Scheduler
→ ResourceManager
```

不得由模块自己创建另一套线程池生命周期。

---

### 3.4 Workflow-Only Reliable Orchestration

需要：

```text
retry
wait
checkpoint
cancel
compensation
crash recovery
```

的多步骤流程全部进入 Workflow。

普通 Script 不承担跨进程可靠恢复职责。

---

### 3.5 Kernel Document 是唯一 Application Source of Truth

未来 OCCT/XDE 即使使用：

```text
TDocStd_Document
OCAF
XCAF
```

也只能作为 Geometry Domain 内部 Document。

Kernel Document 继续管理：

```text
Project
Object identity
Revision
CAM state
Machine context
Environment
Dependency state
Application transaction
```

禁止未来出现：

```text
Kernel Document
     与
OCAF Document
```

两个平级的应用状态真相。

---

### 3.6 一套用户级 Transaction / Undo / Persistence

OCAF、控制器、文件系统等内部事务都不能成为第二套 Application Transaction。

最终用户看到的：

```text
Commit
Undo
Redo
Recovery
```

只能由 Kernel 决定。

---

# 4. Kernel Freeze 前的六个建设阶段

建议不再叫 Phase 10、11、12 无限扩张，而统一定义为：

# Kernel 1.0 Closure

分六个内部 Milestone。

---

# K10A — Execution Contract 1.0

## 目标

彻底冻结 Command / Query 的长期调用协议。

这是当前最高优先级之一。

### A1. Command / Query Version 真正进入请求身份

目前 Descriptor 已经包含 Version，但直接 CommandRequest/QueryRequest 没有版本字段，Registry 也是按 Name 唯一解析。

改为：

```cpp
struct CommandKey {
    CommandName name;
    Version version;
};

struct QueryKey {
    QueryName name;
    Version version;
};
```

或者等效的：

```cpp
CommandRequest {
    CommandName command;
    Version version;
}
```

必须实现：

```text
exact version
compatible version
deprecated version
unsupported version
```

明确行为。

业务模块进入后不得再修改这套身份模型。

---

## A2. Command Scope

当前 Command 默认要求：

```text
ProjectId
DocumentId
```

但未来存在大量非 Document 命令：

```text
app.status
project.new
machine.connect
system.diagnostics
```

因此建立：

```cpp
enum class CommandScope {
    Global,
    Session,
    Project,
    Document
};
```

ExecutionContext：

```cpp
struct ExecutionContext {
    SessionId session;
    optional<ProjectId> project;
    optional<DocumentId> document;
};
```

MachineId、ToolId、ProcessId 等业务身份继续放在领域参数中，不污染通用 Kernel。

---

## A3. Side Effect Execution 正式闭环

这是当前最大的功能缺口。

虽然已有：

```text
ReadOnly
DocumentWrite
FileSystemWrite
Publish
MachineControl
Motion
LaserControl
```

但目前同步 Handler 实际只允许 `DocumentWrite`，异步 Handler 只允许 `ReadOnly`。

因此必须建立统一 Effect 执行协议：

```text
CommandRuntime
      │
      ├── ReadOnly
      │      └→ normal execution
      │
      ├── DocumentWrite
      │      └→ ApplicationTransaction
      │
      └── External Side Effect
             └→ EffectExecutor
```

增加通用：

```cpp
enum class ReplayPolicy {
    Safe,
    Idempotent,
    ReconcileOnly,
    Never
};

enum class RecoveryDisposition {
    Completed,
    Interrupted,
    Indeterminate,
    ReconcileRequired
};
```

Kernel 必须保证：

> Crash Recovery 永远不能自行重放无法证明安全的 Motion、Laser、Machine 等外部副作用。

---

## A4. Effect Guard

Kernel 只提供通用 Guard 契约，例如：

```text
IEffectGuard
```

执行链：

```text
Capability
→ Preconditions
→ EffectGuard
→ Resource lease
→ External execution
```

以后：

```text
MotionSafetyPermit
Collision
Machine interlock
Laser safety
```

由 Machine/Safety Domain 实现。

Kernel 不理解碰撞算法。

---

# K10B — Runtime Project / Document Lifecycle

## 目标

解决当前 Document 只能在 Configuring 阶段加入的问题。

现在 `AppKernel::addDocument()` 只允许 Kernel 启动前调用。

这不足以支持真实桌面 CAD/CAM。

建立：

```text
DocumentRuntime
```

至少支持：

```text
create
attach/open
snapshot
close
detach
remove
list
```

并定义明确状态机：

```text
Detached
→ Opening
→ Open
→ Closing
→ Detached

Opening/Closing
→ Failed
```

必须处理：

```text
关闭时仍存在 Transaction
关闭时存在 Task
关闭时存在 Workflow
持久化失败
Document 重复加载
Project ownership 冲突
```

全部必须 fail-closed。

DocumentStore 继续保持内部存储角色，Host 不直接获得可变 DocumentStore。

---

# K10C — History / Undo / Redo

## 目标

关闭目前已经预留但尚未实现的 Undo 契约。

现在：

```cpp
CommandDescriptor::undoable
```

已经存在；

TransactionCommit 也保存：

```text
before
after
revisionsBefore
revisionsAfter
changes
```



但当前 CommandRegistry 对 `undoable=true` 仍然明确拒绝。

增加：

```text
HistoryRuntime
```

核心模型：

```text
HistoryEntry
HistoryCursor
UndoBarrier
```

Command：

```text
edit.undo
edit.redo
```

本身仍走 CommandRuntime。

第一版严格限制：

> **只有 ApplicationTransaction 内的 DocumentWrite 可以成为普通 Undo。**

以下类型禁止普通 Undo：

```text
MachineControl
Motion
LaserControl
external publish
不可逆 filesystem operation
```

外部副作用若允许恢复，只能使用：

```text
explicit compensation
```

而不是 Undo。

History 与 Journal 必须协调，使正常重启后 History 状态仍然确定，不出现 Revision 与 History cursor 分裂。

---

# K10D — Module Governance 与 Execution Gateway

## 目标

让 Kernel 真正做到“模块可扩展，但无法随意穿透内核”。

目前 ModuleDescriptor 已声明：

```text
services
commands
queries
tasks
events
capabilities
```

但 ModuleRuntime 当前主要严格审计 Service 注册差量。

### D1. ModuleRegistrar

建立：

```cpp
class ModuleRegistrar {
    registerService();
    registerCommand();
    registerQuery();
    registerTask();
    registerWorkflow();
    registerScript();
    registerEvent();
    registerCapability();
};
```

启动结束后自动验证：

```text
Descriptor 声明
       ==
实际注册组件
```

ModuleDescriptor 同步增加：

```text
workflows
scripts
```

---

## D2. 收紧 AppKernel 可变入口

当前 AppKernel 对外暴露多个 Mutable Runtime，包括 `TaskRuntime`，而 TaskRuntime 本身允许直接 submit。

这意味着理论上 Host 可以绕过 Command/Capability。

Kernel 1.0 应增加：

```text
ExecutionGateway
```

Host 只获得：

```text
executeCommand
executeQuery
startWorkflow
executeScript
catalog/discovery
```

原始：

```text
TransactionManager
TaskRuntime::submit
DocumentStore mutable
Registry registration
```

仅限 Kernel/Module composition 内部。

做到：

> “规则要求不得绕过”升级为“类型系统基本无法绕过”。

---

# K10E — Object Type 与 Data Plane 契约

## 目标

为未来 OCCT、刀路、网格、碰撞数据做好底层边界，但不实现业务模型。

当前：

```cpp
ObjectRecord {
    ObjectId;
    ObjectTypeId;
    Value;
}
```



在 Foundation 阶段够用，但长期需要补两项。

---

## E1. ObjectTypeRegistry

定义：

```text
ObjectTypeId
SchemaVersion
Validator
Migration
Reference enumeration
Persistence policy
```

未来：

```text
cad.body@2
cam.operation@4
```

项目升级必须显式迁移，不允许业务模块自行猜测旧 Value。

---

## E2. AssetRef / AssetStore 最小契约

Kernel 已明确禁止把大型几何、网格、刀路作为普通 SQLite BLOB。

因此冻结之前增加最小 Data Plane 契约：

```text
AssetId
ContentDigest
AssetKind
AssetRef
IAssetStore
```

基本规则：

```text
immutable
content-addressable preferred
atomic publish
digest verified
orphan allowed
dangling reference forbidden
```

未来：

```text
STEP
BREP
XCAF
Mesh
Toolpath
BVH
.lmsi
Certificate
```

全部属于 Data Plane。

Kernel Document 只保存：

```text
stable ObjectId
metadata
AssetRef
Revision
```

---

## E3. OCCT 边界提前冻结

只写入架构规则，不接入 OCCT 实现：

```text
Kernel Document = Application Source of Truth

TDocStd_Document = Geometry Domain implementation detail

TopoDS_Shape
TDF_Label
Handle(TDocStd_Document)
```

不得进入 Kernel 公共 API。

同时：

```text
Kernel Transaction/History
```

必须是未来唯一用户级事务。

OCAF Transaction/Undo 只能作为 Geometry Adapter 内部 staging。

---

# K10F — Reliability Certification

这一步不再新增架构能力，只负责证明 Kernel 可以冻结。

## F1. 故障注入

覆盖：

```text
Journal write failure
Snapshot write failure
Hash failure
SQLite transaction failure
Task accept failure
Workflow checkpoint failure
History persistence failure
Asset publish failure
Event delivery failure
Log/Trace exporter failure
```

逐阶段验证：

> 不允许出现部分 Commit 或无法解释的状态。

---

## F2. Crash Recovery

建立独立进程测试：

```text
command crash
transaction crash
task crash
workflow crash
history crash
asset publish crash
external effect crash
```

必须验证：

```text
Document
Revision
Journal
History
Task
Workflow
Idempotency
```

之间一致。

对 External Effect 必须验证：

```text
未知状态
→ Interrupted / Indeterminate
```

而不是重新执行。

---

## F3. 并发与生命周期压力测试

至少覆盖：

```text
并发 Query
并发相同 Idempotency Command
Revision conflict
Task cancellation race
Workflow cancellation race
Document close vs Query
Document close vs Task
shutdown vs active runtime
Module startup failure rollback
```

并进行多轮重复执行，要求：

```text
0 flaky
0 deadlock
0 partial state
```

---

## F4. 内存和性能基线

在业务模块进入前建立固定 Benchmark：

```text
1k Object
10k Object
100k Object
```

测试：

```text
Document snapshot
Transaction begin
Transaction commit
Query framework overhead
Command framework overhead
History undo/redo
Journal append/recovery
```

当前 Document snapshot 和 Transaction staging 存在 ObjectRegistry 按值复制，因此必须用真实 Benchmark 决定是否需要改成：

```text
shared immutable snapshot
+
transaction delta/overlay
```

没有性能问题则不要为了“架构漂亮”提前重构。

---

## F5. 工程质量门禁

Kernel Freeze 前持续要求：

```text
Debug 全测试通过
Release 全测试通过
Production-only build
Architecture boundary scan
连续重复测试
ASan 可用配置
Compiler warnings gate
Fault-injection suite
Independent-process recovery suite
```

Phase 9 当前已经具有 130/130 Debug/Release 测试及重复测试和独立进程 Workflow 恢复验证，因此应该在现有体系上继续扩展，而不是另建测试机制。

---

# 5. Kernel 1.0 Freeze 验收条件

只有下面条件全部满足，才宣布：

```text
Application Kernel 1.0 Ready
```

### Execution

```text
✓ Command/Query 请求版本化
✓ Global/Session/Project/Document scope 完整
✓ ReadOnly / DocumentWrite / ExternalEffect 语义闭环
✓ External side effect 有明确 recovery/replay policy
```

### State

```text
✓ Project/Document 可以运行期创建、打开、关闭
✓ Document 生命周期与 Task/Transaction 一致
✓ Revision 冲突 fail-closed
```

### Transaction

```text
✓ 所有 DocumentWrite 只有一条 Transaction 链
✓ Undo/Redo 正式可用
✓ Journal / History / Revision 一致
```

### Extension

```text
✓ ModuleRegistrar
✓ 全 Registry ownership audit
✓ Registry Ready 后冻结
✓ Host 无法直接绕过 Command 提交 Task/Transaction
```

### Persistence

```text
✓ Idempotency 跨重启
✓ Snapshot/Journal crash-safe
✓ Workflow recovery
✓ History recovery
✓ Asset publish crash-safe
```

### Safety

```text
✓ Capability 默认拒绝
✓ 外部副作用不被 Crash Recovery 自动重放
✓ Unknown 状态 fail-closed
✓ Kernel 不执行 Machine-specific safety 判断
✓ 但提供统一 Effect Guard 接口
```

### Data

```text
✓ ObjectType version/migration 契约
✓ AssetRef/Data Plane 契约
✓ 第三方类型不得进入 Kernel API
```

### Reliability

```text
✓ Debug/Release 全绿
✓ 无 flaky test
✓ 故障注入全绿
✓ 生命周期压力测试全绿
✓ 性能基线建立并记录
```

---

# 6. 明确“不进入 Kernel 1.0 收口”的内容

为了防止再次无限扩张，本轮明确不实现：

```text
OCCT / OCAF / XDE
CAD Modeling
CAM Algorithm
Toolpath
Collision
Machine Kinematics
Controller
MotionSafetyPermit
.lmsi
GTN
Qt
Ribbon
产品 CLI parser
Python/Lua
RPC
AI Agent
动态 Plugin Loader
Digital Twin
OpenTelemetry
```

特别要注意：

> **开放式命令行的 Parser、Alias、交互输入、自动补全属于 Command Surface / Host，而不是 Application Kernel。**

Kernel 只保证：

```text
Command Catalog
Descriptor
Schema
Version
Execution
Security
Transaction
```

这样以后无论：

```text
Qt Command Line
CLI
Python
RPC
AI
```

都可以把输入转换为同一个结构化 CommandRequest。

---

# 7. 最终内核结构

Kernel 1.0 收口完成后的逻辑结构建议冻结为：

```text
                    Hosts
       GUI / CLI / RPC / Script / AI
                      │
                      ▼
              ExecutionGateway
                      │
       ┌──────────────┼──────────────┐
       ▼              ▼              ▼
    Command          Query        Workflow
       │
       ▼
              Application Kernel
┌──────────────────────────────────────────┐
│ Foundation                               │
│                                          │
│ Command / Query Runtime                  │
│ Execution Context / Capability           │
│ ApplicationTransaction / History         │
│ DocumentRuntime / Revision               │
│ Task / Scheduler / Resource              │
│ Workflow / Script                        │
│ Event                                    │
│ Persistence / Recovery                   │
│ ObjectType / AssetRef                    │
│ Observability                            │
│ ModuleRuntime / ModuleRegistrar          │
└──────────────────────────────────────────┘
                      │
                      ▼
              Domain Modules
```

Domain Modules 才开始：

```text
Geometry
geometry_occ
CAD
CAM
Machine
Collision
Process
Project UI
```

---

# 8. 推荐实施顺序

不要并行铺开，严格按依赖关系推进：

```text
K10A
Execution Contract
     ↓
K10B
Document Lifecycle
     ↓
K10C
History / Undo
     ↓
K10D
Module Governance / Gateway
     ↓
K10E
ObjectType / Asset Boundary
     ↓
K10F
Reliability Certification
     ↓
Kernel 1.0 Freeze
     ↓
开始业务模块
```

其中优先级最高的是：

```text
1. Command Version / Scope
2. External Effect Runtime
3. Runtime Document Lifecycle
4. Undo/Redo
5. ModuleRegistrar / ExecutionGateway
```

这五项属于真正的 P0。

ObjectType、AssetRef 和性能优化属于最后的结构性收口，其中性能部分必须遵循：

> **先 Benchmark，再决定是否重构。**

---

# 9. Freeze 后的变更政策

Kernel 1.0 冻结以后，业务开发原则上不得修改 Kernel。

只有以下情况允许开启 Kernel RFC：

```text
安全漏洞
数据一致性错误
Crash Recovery 无法保证
公共契约存在根本错误
无法表达一整类业务，而不是某一个业务功能
```

以下理由不得修改 Kernel：

```text
“CAM 这样写更方便”
“OCCT 这个 API 不好包装”
“GUI 想直接拿 Document”
“控制器直接调用性能更高”
“某个模块想创建自己的线程”
“脚本想绕过 Command”
```

这些都应该由 Domain/Adapter 解决。

---

# 10. 最终判断

完成现有 Phase 1–9 后，LaserCNC 已经具备一个非常扎实的 Application Kernel。

现在最正确的方向不是继续往内核中加入更多框架，而是完成最后几个关键闭环：

```text
Execution Contract
External Effect
Document Lifecycle
Undo/Redo
Module Governance
Data Plane Boundary
Reliability Certification
```

完成这些后，Kernel 就应该进入长期冻结状态。

此时可以认为：

> **LaserCNC Kernel 已达到业务模块接入前的安全、稳定、成熟状态，能够长期支撑 OCCT、CAD、CAM、五轴运动、安全碰撞、加工 Process、CLI、脚本以及未来 AI 自动化，而无需改变核心执行与状态一致性模型。**
