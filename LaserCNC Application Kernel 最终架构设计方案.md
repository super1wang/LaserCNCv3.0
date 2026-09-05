# LaserCNC Application Kernel 最终架构设计方案
## —— Command-First / Automation-First / Infrastructure-Adapter Architecture

> 2026-09-05 收口状态：实际执行以 [ST1C 补充执行计划](docs/内核契约/ST1C-补充审计与剩余执行计划.md) 为准。C6c1 已建立共享 Value/Schema/JSON/TOML 硬预算，[C6c2a](docs/内核契约/ST1C6c2a-StrongId统一字节与编码准入.md) 已统一 35 类 StrongId 的 4096 字节与严格 UTF-8 准入；注册定义、运行期消息/观察及持久材料累计预算仍由 C6c2b–d 承接。C6c2a 不代表 C6c、C6 或 Kernel 1.0 已冻结，且本阶段仍只覆盖内核。

---

# 1. 设计定位

本次重构不再以 CAD、CAM 或 GUI 为软件中心，而是建立一套长期稳定的：

> **Application Platform Kernel**

核心目标是让：

- Qt GUI
- 内置命令行
- 外部 CLI
- 自动化脚本
- CTest / CI
- AI Agent
- RPC / MCP / WebSocket
- 未来 MES / 数字孪生 / 远程控制

全部建立在同一套应用运行模型之上。

整体原则：

> GUI、CLI、Script、AI 都只是 Host / Frontend。  
> Command / Query / Workflow 才是业务行为入口。  
> CAD、CAM、Machine、Collision 等只是建立在 Kernel 上的领域模块。  
> 第三方库只提供基础设施实现，不允许定义 Kernel 的产品语义。

因此，本次重构的真正目标不是：

> 给 CAD/CAM 增加命令行。

而是：

> 构建一套 Command-First、Automation-First、Headless-First、Testable-First、Infrastructure-Decoupled 的工业应用内核。

---

# 2. 总体架构

```text
┌───────────────────────────────────────────────────────────────┐
│                         Frontends                             │
│                                                               │
│   Qt GUI   Internal CLI   External CLI   Script   AI / RPC   │
└───────────────────────────┬───────────────────────────────────┘
                            │
                            ▼
┌───────────────────────────────────────────────────────────────┐
│                    Automation Gateway                         │
│                                                               │
│ CLI Parser │ Script Adapter │ JSON-RPC │ AI Adapter │ MCP    │
└───────────────────────────┬───────────────────────────────────┘
                            │
                            ▼
┌──────────────────── Application Kernel ───────────────────────┐
│                                                               │
│ Runtime                                                       │
│ ├─ CommandRuntime                                             │
│ ├─ QueryRuntime                                               │
│ ├─ WorkflowRuntime                                            │
│ ├─ TaskRuntime                                                │
│ ├─ Scheduler                                                  │
│ └─ TransactionManager                                         │
│                                                               │
│ State                                                         │
│ ├─ DocumentStore                                              │
│ ├─ ObjectRegistry                                             │
│ ├─ RevisionManager                                            │
│ ├─ SnapshotManager                                            │
│ └─ Journal                                                    │
│                                                               │
│ Messaging / Observability                                     │
│ ├─ EventBus                                                   │
│ ├─ LogService                                                 │
│ ├─ TraceService                                               │
│ ├─ MetricsService                                             │
│ └─ Diagnostics                                                │
│                                                               │
│ Platform                                                      │
│ ├─ ServiceRegistry                                            │
│ ├─ ModuleRuntime                                              │
│ ├─ ConfigService                                              │
│ ├─ PersistenceService                                         │
│ ├─ CapabilityService                                          │
│ ├─ ResourceManager                                            │
│ ├─ Clock                                                      │
│ ├─ IdGenerator                                                │
│ └─ FileSystem                                                 │
└───────────────────────────┬───────────────────────────────────┘
                            │
                            ▼
                 Kernel Infrastructure Interfaces
                            │
                 ─── Third Party Boundary ───
                            │
┌──────────────────────── Infrastructure ───────────────────────┐
│                                                               │
│ spdlog │ toml11 │ jsoncons │ SQLite │ BS::thread_pool        │
│                                                               │
│ Optional: Zstd │ Taskflow │ OpenTelemetry │ Hash Backend      │
└───────────────────────────┬───────────────────────────────────┘
                            │
                            ▼
┌──────────────────── Domain Modules ───────────────────────────┐
│                                                               │
│ CAD │ CAM │ Machine │ Collision │ Simulation │ Process ...   │
└───────────────────────────────────────────────────────────────┘
```

---

# 3. 最核心的架构原则

整个项目必须长期遵守三个边界。

## 3.1 Kernel 不知道 CAD/CAM

Kernel 禁止依赖：

```text
CAD
CAM
OCCT
Machine Controller
Collision Engine
Qt Widgets
```

禁止出现：

```cpp
kernel.cad();
kernel.cam();
kernel.machine();
```

只提供：

```cpp
kernel.commands();
kernel.queries();
kernel.tasks();
kernel.events();
kernel.services();
kernel.transactions();
kernel.config();
kernel.logs();
```

CAD/CAM 等模块通过注册方式加入系统。

---

## 3.2 Domain 不知道具体第三方基础设施

例如 CAD/CAM 代码中禁止直接：

```cpp
spdlog::info(...);
sqlite3_exec(...);
toml::parse(...);
BS::thread_pool pool;
jsoncons::json value;
```

必须通过 Kernel 接口：

```text
ILogService
IPersistenceService
IConfigService
ITaskExecutor
IValueSerializer
ISchemaValidator
```

调用。

---

## 3.3 第三方类型禁止穿透 Kernel Public API

以下类型不得出现在 Kernel / Domain 公共头文件：

```text
spdlog::logger
sqlite3*
toml11 value
jsoncons::json
BS::thread_pool
Taskflow types
OpenTelemetry types
```

第三方库必须被限制在：

```text
infrastructure/
```

内部。

---

# 4. Kernel 核心运行模型

整个 Application Kernel 收敛为六个主要执行概念：

```text
Command
Query
Workflow
Task
Transaction
Event
```

状态模型：

```text
Document
Object
Revision
Snapshot
Journal
```

关系：

```text
                 ┌───────────┐
                 │ Command   │
                 └─────┬─────┘
                       │
                       ▼
                 Transaction
                       │
                       ▼
                    Document
                       │
                       ▼
                    Revision
                       │
                       ▼
                     Event


Query ───────────────▶ Snapshot


Workflow
    │
    ├── Query
    ├── Command
    ├── Task
    ├── Condition
    ├── Parallel
    └── Wait


Task
    │
    ├── Snapshot Input
    ├── Background Compute
    └── Revision-Checked Commit
```

---

# 5. AppKernel

建议接口：

```cpp
class AppKernel
{
public:
    CommandRuntime& commands();
    QueryRuntime& queries();
    WorkflowRuntime& workflows();

    TaskRuntime& tasks();
    Scheduler& scheduler();

    TransactionManager& transactions();

    EventBus& events();

    ServiceRegistry& services();
    ModuleRuntime& modules();

    ConfigService& config();
    PersistenceService& persistence();

    LogService& logs();
    TraceService& traces();
    MetricsService& metrics();
    DiagnosticsService& diagnostics();

    CapabilityService& capabilities();
};
```

AppKernel 作为：

> Application Composition Root

负责组装系统。

业务代码不鼓励频繁直接访问 ServiceRegistry。

推荐构造注入：

```cpp
CamGenerateCommand::CamGenerateCommand(
    ICamToolpathService& toolpath,
    IDocumentService& document);
```

而不是：

```cpp
kernel.services().resolve<ICamToolpathService>();
```

遍布整个项目。

---

# 6. Command Runtime

Command 表示：

> 对系统产生业务状态变化的操作。

例如：

```text
project.new
project.save

cad.feature.hole.create
cad.boolean.cut
cad.object.delete

cam.operation.create
cam.toolpath.generate
cam.publish
```

Command 应对应稳定业务语义，而不是底层实现函数。

不应该暴露：

```text
occ.make_edge
occ.make_wire
occ.brep_cut_internal
```

而应该：

```text
cad.feature.hole.create
cad.boolean.cut
```

---

# 7. Command Descriptor

```cpp
struct CommandDescriptor
{
    CommandName name;
    Version version;

    Schema arguments;
    Schema result;

    ExecutionMode executionMode;

    SideEffectLevel sideEffect;
    Capability capability;

    bool undoable;
    bool deterministic;
    bool idempotent;
};
```

SideEffect 示例：

```text
ReadOnly
DocumentWrite
FileSystemWrite
Publish
MachineControl
Motion
LaserControl
```

---

# 8. Command Request

```cpp
struct CommandRequest
{
    RequestId requestId;

    SessionId sessionId;
    ProjectId projectId;

    CommandName command;
    Value arguments;

    std::optional<Revision> expectedRevision;

    CorrelationId correlationId;
    TraceId traceId;

    std::optional<IdempotencyKey> idempotencyKey;
};
```

注意：

这里使用的是：

```text
Kernel Value
```

而不是：

```text
jsoncons::json
nlohmann::json
QJsonObject
```

---

# 9. Kernel Value Model

建议 Kernel 自己维护一个非常轻量的数据值模型：

```cpp
class Value
{
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;

    using Storage = std::variant<
        std::nullptr_t,
        bool,
        int64_t,
        double,
        std::string,
        Array,
        Object
    >;
};
```

它负责：

```text
Command Arguments
Command Result
Query Result
Event Data
Workflow Variables
RPC Payload
Structured Log Data
```

但：

> Kernel 自己只拥有数据模型，不自己实现 JSON/TOML Parser。

解析和序列化交给 Infrastructure Adapter。

这是避免第三方类型污染 Kernel 的关键设计。

---

# 10. Command 标准执行链

```text
Command Request
      ↓
CommandRegistry
      ↓
Schema Validation
      ↓
Capability Check
      ↓
Precondition Check
      ↓
Begin Transaction
      ↓
Command Handler
      ↓
State Changes
      ↓
Collect Domain Events
      ↓
Commit Transaction
      ↓
Revision Increment
      ↓
Publish Events
      ↓
Structured Log / Trace
      ↓
Command Result
```

任何公开业务状态修改不得绕过该链路。

---

# 11. Query Runtime

Query：

> 只读取系统状态，不产生业务副作用。

例如：

```text
project.status

cad.object.get
cad.object.list

cam.operation.get
cam.toolpath.statistics

task.status
machine.status
```

原则：

```text
Command = Write
Query   = Read
```

Query 尽量基于：

```text
Immutable Snapshot
```

执行。

AI 典型流程应该是：

```text
Query
↓
Query
↓
Plan
↓
Command
↓
Query
↓
Command
```

---

# 12. Workflow Runtime

Workflow 用于：

> 跨多个 Command / Query / Task 的业务流程。

例如：

```text
创建圆管
↓
创建钻孔
↓
创建 CAM Setup
↓
创建 Operation
↓
生成刀路
↓
碰撞验证
↓
发布
```

支持：

```text
Variables
Dependency
Condition
Parallel
Wait
Timeout
Retry
Cancellation
Checkpoint
Compensation
```

AI：

```text
Natural Language
       ↓
AI Planner
       ↓
Execution Plan
       ↓
Workflow Runtime
       ↓
Command / Query / Task
```

WorkflowRuntime 属于 LaserCNC 核心产品语义。

禁止被 Taskflow、Temporal 等第三方框架直接定义。

---

# 13. Task Runtime

长耗时操作统一进入 TaskRuntime。

例如：

```text
STEP Import
Geometry Healing
Boolean
Assembly Processing
Toolpath Generation
Collision Detection
Simulation
Safety Certification
LMSI Build
Post Processing
```

统一状态：

```text
Pending
Ready
Running
Succeeded
Failed
CancelRequested
Cancelled
Stale
```

统一：

```text
CancellationToken
Deadline
ProgressReporter
TraceContext
ResourceContext
```

禁止模块自行创建：

```text
atomic_bool stop;
bool quit;
abortFlag;
```

---

# 14. Scheduler 与第三方线程池边界

TaskRuntime 自己定义任务语义。

推荐：

```text
TaskRuntime
      ↓
Scheduler
      ↓
ITaskExecutor
      ↓
BsThreadPoolExecutor
      ↓
BS::thread_pool
```

其中：

```text
TaskRuntime      = 自研
Scheduler        = 自研
Resource Model   = 自研
BS::thread_pool  = 第三方执行 backend
```

不能直接：

```text
TaskRuntime = BS::thread_pool
```

因为线程池不知道：

```text
TaskId
Revision
Snapshot
Project resource
Machine resource
Workflow dependency
Task persistence
```

---

# 15. Scheduler Resource Model

Task 声明：

```text
CPU
DiskIO
GPU
OCCT
ProjectRead
ProjectWrite
MachineController
CollisionBackend
```

例如：

```text
STEP Import
    CPU = Heavy
    IO = Heavy

Boolean
    CPU = Heavy
    OCCT = Exclusive/Shared

Toolpath Generation
    CPU = Heavy
    Project = Read

Project Save
    IO = Heavy
    Snapshot = Read
```

Scheduler 根据：

```text
Dependency
Priority
Resource
Cancellation
Deadline
```

决定执行。

---

# 16. TransactionManager

所有业务状态修改必须在 Application Transaction 中完成。

例如：

```text
创建 CAM Operation
+
创建参数
+
绑定刀具
+
绑定 Geometry
+
修改工程树
+
标记 Toolpath Dirty
```

必须：

```text
全部成功
或者
全部失败
```

执行：

```text
Begin
 ↓
Modify
 ↓
Collect Events
 ↓
Commit
 ↓
Revision++
 ↓
Publish Events
```

---

# 17. Application Transaction 与 SQLite Transaction

两者不能混淆。

```text
Application Transaction
        │
        ├─ Document mutation
        ├─ Revision
        ├─ Undo
        ├─ Domain Events
        └─ Persistence Transaction
                    │
                    ▼
                SQLite
```

SQLite Transaction 只是持久化 backend。

Application Transaction 才是业务一致性边界。

---

# 18. Revision 一致性模型

至少支持：

```text
ProjectRevision
DocumentRevision
GeometryRevision
CamRevision
MachineContextRevision
EnvironmentRevision
```

后台任务：

```text
Capture Revision 121
       ↓
Background Compute
       ↓
Current Revision 123
       ↓
Result = STALE
```

禁止覆盖当前状态。

因此后台 Task 固定采用：

```text
Capture Immutable Snapshot
       ↓
Background Compute
       ↓
Generate Result
       ↓
Revision Check
       ↓
Atomic Commit
```

---

# 19. ObjectRegistry 与稳定 ID

所有公开对象必须使用稳定 ID：

```text
ProjectId
ObjectId
BodyId
FeatureId
OperationId
ToolId
ToolpathId
TaskId
WorkflowId
```

禁止：

```text
TopoDS_Shape*
AIS_Shape*
QObject*
Face[12]
Edge[17]
裸指针
数组位置
```

作为持久身份。

推荐：

```cpp
template<typename Tag>
class StrongId
{
    Uuid value_;
};
```

StrongId 可以轻量自研。

无需仅为了 UUID 引入完整 Boost。

---

# 20. Snapshot + Journal

项目持久化采用：

```text
Snapshot
+
Command / State Journal
```

例如：

```text
Snapshot Revision 100
       │
       ├─ Change 101
       ├─ Change 102
       ├─ Change 103
       └─ Change 104
```

可以支持：

```text
Crash Recovery
Auto Save
Undo / Redo
Replay
Regression Reproduction
Audit
AI Operation History
```

---

# 21. SQLite 作为持久化基础设施

推荐将 SQLite 正式加入 Production Core。

SQLite 负责：

```text
Project Metadata
Schema Migration
Journal
Workflow State
Task History
Idempotency Record
Snapshot Index
Object Metadata
Diagnostics Metadata
```

例如：

```text
kernel.db
```

结构：

```text
schema_migrations
command_journal
workflow_instances
workflow_steps
task_history
idempotency_records
snapshot_index
object_metadata
project_metadata
```

---

# 22. SQLite 不保存大型几何资产

禁止把：

```text
STEP
BREP
Mesh
Toolpath
LMSI
Collision Cache
```

全部以大型 BLOB 塞进数据库。

推荐：

```text
Project/
│
├── kernel.db
│
├── geometry/
├── snapshots/
├── cache/
├── toolpath/
└── artifacts/
```

数据库保存：

```text
ID
Path
Hash
Revision
Type
Metadata
Dependency
```

即：

```text
SQLite = Control Plane
Files  = Data Plane
```

---

# 23. SQLite Adapter

第一阶段不建议再额外加入大型 ORM。

不优先使用：

```text
ODB
SOCI
sqlite_orm
大型数据库框架
```

可以自己实现一层很薄的 RAII Wrapper：

```text
SqliteConnection
SqliteStatement
SqliteTransaction
SqliteRow
```

这些只存在于：

```text
infrastructure/persistence/sqlite/
```

属于 glue code。

---

# 24. EventBus

Event 与 Log 必须严格分离。

Event：

> 已发生的系统事实。

例如：

```text
ProjectCreated
ObjectCreated
FeatureChanged
TaskCompleted
ToolpathGenerated
```

分三类。

## Domain Event

```text
ObjectCreated
ToolpathGenerated
```

Transaction commit 后才能发布。

## Notification

```text
TaskProgress
SelectionChanged
ViewportChanged
```

允许合并、丢弃。

## System Event

```text
ModuleStarted
SessionOpened
FatalError
```

---

# 25. EventBus 采用轻量自研

不建议为了 EventBus 引入：

```text
Boost.Signals2
RxCpp
EnTT Dispatcher
完整 Reactive Framework
```

核心需求：

```cpp
subscribe<Event>()
publish(Event)
unsubscribe()
```

再增加：

```text
ImmediateDelivery
QueuedDelivery
NotificationCoalescing
SubscriptionLifetime
TracePropagation
```

即可。

由于 EventBus 与：

```text
Transaction Commit
Domain Event
Notification
Trace
```

深度关联，因此属于 Kernel 产品语义。

---

# 26. Logging Infrastructure

日志 backend 使用：

```text
spdlog
```

推荐结构：

```text
ILogService
     ↓
SpdlogLogService
     ↓
spdlog
     ├─ Console Sink
     ├─ Rotating File Sink
     └─ JSONL Sink
```

业务模块：

```cpp
ctx.logs().info(
    "cam.toolpath",
    "Toolpath generation started",
    fields);
```

禁止：

```cpp
spdlog::info(...);
```

直接散落业务模块。

---

# 27. Structured Logging

每条日志至少包含：

```text
timestamp
level

session_id
project_id

command_id
task_id
workflow_id

correlation_id
trace_id

module
category
thread_id

message
structured_data
```

输出支持：

```text
Human Readable
JSONL
```

CTest、Codex、CLI 和 AI 都可以读取 JSONL。

---

# 28. Trace / Metrics

平台 Observability：

```text
Log
Trace
Metric
Diagnostics
```

例如：

```text
Trace AI-ABC

AI Plan
└─ Workflow
   ├─ Query
   ├─ Command
   │  └─ CAD Boolean
   ├─ Command
   │  └─ CAM Operation
   └─ Task
      ├─ Geometry
      ├─ Toolpath
      └─ Collision
```

第一阶段自研：

```text
LocalTraceService
LocalMetricsService
```

输出：

```text
Memory
JSONL
SQLite Metadata
```

---

# 29. OpenTelemetry 后置

当前不直接引入 OpenTelemetry SDK。

先定义：

```text
ITraceService
IMetricsService
ITraceExporter
IMetricsExporter
```

以后需要：

```text
远程诊断
MES
数字孪生
Cloud
Distributed Tracing
```

再增加：

```text
OpenTelemetryAdapter
```

避免第一阶段引入过重依赖。

---

# 30. ConfigService

全项目禁止散落：

```text
QSettings
INI Parser
随机 JSON Config
全局变量
```

统一：

```text
ConfigService
```

配置层级：

```text
Defaults
↓
System
↓
Machine
↓
User
↓
Project
↓
Session
↓
Runtime Override
```

配置项具有：

```text
Type
Default
Min
Max
Unit
Description
RestartRequired
```

---

# 31. TOML Backend

配置文件采用：

```text
TOML
```

Infrastructure：

```text
IConfigSerializer
      ↓
TomlConfigAdapter
      ↓
toml11
```

TOML 仅用于：

```text
Human Editable Configuration
```

不用于：

```text
Command Protocol
Project Serialization
Journal
RPC
```

因此明确：

```text
Human Config     = TOML
Machine Protocol = JSON
Persistent State = SQLite + Files
```

---

# 32. JSON / Schema Infrastructure

Command / Query / Event / RPC 需要：

```text
JSON
JSON Schema
JSON Pointer
JSON Patch
Schema Validation
```

推荐基础库：

```text
jsoncons
```

但 jsoncons 不进入公共 API。

结构：

```text
Kernel Value
     ↓
IValueSerializer
ISchemaValidator
     ↓
JsonconsAdapter
     ↓
jsoncons
```

---

# 33. Schema

Kernel 自己定义：

```text
Schema
SchemaId
SchemaVersion
FieldDescriptor
CommandDescriptor
QueryDescriptor
EventDescriptor
```

但具体：

```text
JSON Schema parsing
JSON Schema validation
JSON serialization
```

交给第三方。

这样：

> Kernel 拥有 Schema 语义，第三方拥有 Parser / Validator 实现。

---

# 34. CLI Host

CLI argv 解析使用：

```text
CLI11
```

结构：

```text
CLI11
  ↓
CliHost
  ↓
CommandEnvelope
  ↓
CommandRuntime
```

CLI11 只处理：

```text
argv
options
subcommands
help
shell input
```

真正参数 Schema 校验仍由：

```text
CommandRuntime
```

完成。

---

# 35. Command Discovery

支持：

```text
command.list
command.describe

query.list
query.describe

workflow.list
workflow.describe

module.list
module.describe

capability.list
```

例如：

```text
lasercnc-cli command describe cad.feature.hole.create
```

返回机器可读 Schema。

这同时成为：

```text
CLI Help
Script API
AI Tool Description
MCP Tool Description
RPC Contract
```

的统一来源。

---

# 36. ModuleRuntime

即使所有模块静态链接，也必须具有 Module Runtime。

生命周期：

```text
Discovered
↓
Registered
↓
Initialized
↓
Started
↓
Ready
↓
Stopping
↓
Stopped
```

模块描述：

```text
Name
Version
Dependencies
Required Services
Provided Services

Commands
Queries
Tasks
Events
Capabilities
```

ModuleRuntime 建立依赖 DAG，并检测：

```text
Missing Dependency
Version Conflict
Circular Dependency
```

---

# 37. ModuleRuntime 自研，不引入重型插件框架

当前不需要引入：

```text
CppMicroServices
Boost.DI
Google Fruit
大型 IoC
OSGi-like framework
```

原因：

```text
不要求运行时动态插件
模块依赖较稳定
追求可控和长期维护
```

只需要实现：

```text
ServiceRegistry
ModuleRegistry
ModuleDescriptor
DependencyGraph
Lifecycle
```

即可。

---

# 38. Capability / Security

Command 定义 Capability：

```text
cad.read
cad.modify

cam.read
cam.generate
cam.publish

project.read
project.write

machine.read
machine.control
machine.motion

laser.enable
```

不同 Session：

```text
GUI
CLI
Script
AI
Remote Client
Maintenance
```

拥有不同 Capability。

AI 默认可以：

```text
cad.*
cam.generate
cam.verify
```

但不应默认拥有：

```text
machine.motion
laser.enable
```

---

# 39. Idempotency 与 Preconditions

外部控制必须支持：

```text
IdempotencyKey
ExpectedRevision
```

例如：

```text
AI-PLAN-81-STEP-12
```

重复请求：

```text
直接返回原结果
```

而不是重复执行。

并发修改时：

```text
expectedRevision = 102
currentRevision = 105
```

返回：

```text
RevisionConflict
```

---

# 40. Error Model

Kernel 统一：

```cpp
struct Error
{
    ErrorCode code;
    ErrorCategory category;

    Severity severity;

    std::string message;
    Value details;

    std::shared_ptr<Error> cause;
};
```

禁止混用：

```text
false
nullptr
-1
QString error
裸 exception
```

第三方错误：

```text
SQLite Error
spdlog failure
JSON parse error
TOML parse error
thread pool error
```

必须在 Infrastructure Adapter 边界转换成：

```text
Kernel Error
```

---

# 41. Error 示例

```text
Runtime.CommandNotFound
Runtime.SchemaInvalid

Project.RevisionConflict

CAD.Boolean.Failed
CAD.Geometry.Invalid

CAM.Toolpath.GenerationFailed

Task.Cancelled
Task.Timeout

Persistence.DatabaseFailed

Config.ParseFailed

Machine.NotReady
```

---

# 42. RPC 第一阶段保持轻量

外部 CLI 与运行中的 GUI 通信：

第一阶段推荐：

```text
QLocalSocket / Named Pipe
+
Length-Prefixed JSON
```

结构：

```text
External CLI
      ↓
Local IPC
      ↓
CommandEnvelope
      ↓
CommandRuntime
```

目前不需要引入：

```text
gRPC
protobuf
HTTP/2 stack
```

---

# 43. RPC 后续扩展

未来如果真正需要：

```text
LAN
MES
Cloud
Remote SDK
跨语言客户端
```

再增加：

```text
gRPC Adapter
WebSocket Adapter
MCP Adapter
```

Kernel 不受影响。

---

# 44. Script Runtime

第一阶段：

```text
Command
Query
Variables
Result Binding
Wait
Assert
If
ForEach
Include
```

例如：

```text
project.new name="tube_demo"

cad.primitive.tube.create \
    outerDiameter=100 \
    wallThickness=3 \
    length=500 \
    out=tube

cad.feature.hole.create \
    target=${tube.body} \
    diameter=8 \
    out=hole

cam.toolpath.generate \
    operation=${operation} \
    wait=true
```

以后如果需要复杂逻辑再接：

```text
Python
Lua
JavaScript
```

但这些语言只能通过：

```text
Command
Query
Workflow
```

访问系统。

---

# 45. AI Runtime

AI：

```text
Natural Language
       ↓
AI Planner
       ↓
Execution Plan
       ↓
Schema Validate
       ↓
Dry Run
       ↓
Workflow Runtime
       ↓
Command / Query / Task
```

AI 不允许：

```text
直接操作 OCC
直接修改 Document
直接绕过 CommandRuntime
直接访问数据库
直接控制硬件
```

---

# 46. Testing

测试分两层。

## Unit / Component

推荐：

```text
Catch2
```

作为 Development Dependency。

用于：

```text
StrongId
Value
CommandRegistry
Transaction
Revision
EventBus
Scheduler
```

---

## Integration / Process

使用：

```text
CTest
```

调用：

```text
Headless Host
CLI Host
Command Runtime
```

例如：

```text
ctest -R cad_create_tube -V
```

形成：

```text
CTest
 ↓
Headless
 ↓
CommandRuntime
 ↓
Real CAD/CAM
```

---

# 47. Performance Benchmark

建议加入：

```text
Google Benchmark
```

但仅作为开发依赖。

长期监控：

```text
Command Dispatch
Query Dispatch
Event Dispatch
ObjectRegistry Lookup
Journal Append
Task Scheduling
Value Serialization
```

防止 Kernel 性能在长期重构中缓慢退化。

---

# 48. Compression

第一阶段不强制引入压缩。

预留：

```text
ICompressionCodec
```

默认：

```text
NoneCodec
```

未来：

```text
ZstdCodec
```

用于：

```text
Snapshot
Journal Archive
Toolpath
Mesh Cache
Safety Package
```

---

# 49. Hash

Kernel 定义：

```text
IHashService
ContentDigest
```

避免公开：

```text
xxHash
SHA256
BLAKE3
```

具体实现类型。

用途：

```text
Geometry Fingerprint
Machine Package
Workpiece Hash
Toolpath Hash
Safety Certificate
Snapshot Hash
Cache Identity
```

涉及持久安全身份时应使用强内容摘要，而不能只依赖普通 HashMap hash。

---

# 50. Units

Kernel Schema 支持：

```text
unit = "mm"
unit = "deg"
unit = "mm/s"
```

但不建议让复杂 Units Template Library 渗透整个 Kernel。

如果未来：

```text
CAM
Machine
Motion
Geometry
```

需要强类型单位，可在领域层评估：

```text
mp-units
```

而不是：

```text
CommandRuntime
```

全局依赖它。

---

# 51. 推荐 Production Dependencies

正式核心依赖控制在：

```text
spdlog
toml11
jsoncons
SQLite
BS::thread_pool
```

即：

> 五个主要 Production Infrastructure Dependencies。

---

# 52. Host Dependencies

```text
CLI11
```

目前只属于：

```text
hosts/cli
```

不属于 Kernel。

---

# 53. Development Dependencies

```text
Catch2
Google Benchmark
```

只用于：

```text
tests/
benchmarks/
```

不进入正式运行时。

---

# 54. Optional Dependencies

按需启用：

```text
Taskflow
OpenTelemetry
Zstd
BLAKE3 / SHA backend
mp-units
gRPC
```

原则：

> 没有明确使用场景之前不提前加入。

---

# 55. 为什么暂不引入 Taskflow

Taskflow 非常适合复杂 DAG：

```text
         Mesh
        /    \
     Offset   BVH
        \    /
        Verify
          ↓
       Publish
```

但当前：

```text
WorkflowRuntime
TaskRuntime
Scheduler
```

拥有大量 LaserCNC 自有语义。

因此第一阶段：

```text
BS::thread_pool
```

只提供 Executor。

未来如果真正出现复杂 DAG，可以：

```text
ITaskExecutor
 ├─ BsExecutor
 └─ TaskflowExecutor
```

不改变 TaskRuntime API。

---

# 56. 哪些能力必须自己实现

以下属于产品语义，必须自己掌控：

```text
StrongId
Value
Result / Error

CommandRuntime
QueryRuntime
WorkflowRuntime

TaskRuntime
Scheduler
Resource Model

TransactionManager
RevisionManager

Document
ObjectRegistry

EventBus

ServiceRegistry
ModuleRuntime

Capability
Schema Descriptor

Snapshot Semantics
Journal Semantics

Diagnostics
```

---

# 57. 哪些能力应该交给第三方

成熟基础能力不要重复造轮子：

```text
Logging Engine        → spdlog
TOML Parser           → toml11
JSON Parser           → jsoncons
JSON Schema Validator → jsoncons
Embedded Database     → SQLite
Thread Pool Executor  → BS::thread_pool
CLI Parsing           → CLI11
Unit Test Framework   → Catch2
Benchmark             → Google Benchmark
Compression           → Zstd（按需）
Telemetry Export      → OpenTelemetry（按需）
```

---

# 58. Infrastructure Adapter 层

正式增加：

```text
infrastructure/
```

目录。

结构：

```text
infrastructure/

    logging/
        spdlog/

    config/
        toml11/

    serialization/
        jsoncons/

    persistence/
        sqlite/

    execution/
        bs_thread_pool/

    compression/
        zstd/              # optional

    telemetry/
        opentelemetry/     # optional

    hashing/
        blake3_or_sha/     # optional
```

---

# 59. Infrastructure Adapter 依赖规则

只能：

```text
Kernel Interface
      ↓
Infrastructure Adapter
      ↓
Third Party
```

禁止：

```text
Domain
 ↓
Third Party
```

例如正确：

```text
CAM
 ↓
ILogService
 ↓
SpdlogAdapter
 ↓
spdlog
```

错误：

```text
CAM
 ↓
spdlog
```

---

# 60. 最终源码目录

```text
src/

    foundation/
        ids/
        value/
        result/
        error/
        schema/
        serialization/

    kernel/
        app_kernel/
        service_registry/
        module_runtime/
        lifecycle/

    runtime/
        command/
        query/
        workflow/
        task/
        scheduler/
        transaction/

    state/
        document/
        object_registry/
        revision/
        snapshot/
        journal/

    messaging/
        event/
        notification/

    platform/
        config/
        capability/
        persistence/
        filesystem/
        clock/
        resource/
        hashing/
        compression/

    observability/
        logging/
        tracing/
        metrics/
        diagnostics/

    infrastructure/

        logging/
            spdlog/

        config/
            toml11/

        serialization/
            jsoncons/

        persistence/
            sqlite/

        execution/
            bs_thread_pool/

        compression/
            zstd/

        telemetry/
            opentelemetry/

        hashing/
            content_hash/

    automation/
        protocol/
        cli/
        script/
        rpc/
        ai/

    modules/

        cad/
            document/
            geometry/
            feature/
            selection/
            transform/
            boolean/
            import_export/

        cam/
            setup/
            tool/
            geometry/
            operation/
            strategy/
            toolpath/
            simulation/
            verification/
            publish/

        collision/
        simulation/
        machine/
        process/

    hosts/
        gui/
        cli/
        headless/

tests/

    unit/
    command/
    integration/
    workflow/
    regression/
    performance/
    scripts/

benchmarks/
```

---

# 61. 第三方依赖治理

所有依赖统一管理。

禁止各模块自行：

```cmake
FetchContent_Declare(...)
```

推荐集中：

```text
cmake/
    Dependencies.cmake
```

或者采用：

```text
vcpkg.json
```

Manifest Mode。

---

# 62. 版本锁定

禁止：

```text
master
main
latest
```

正式依赖必须固定：

```text
Version
Tag
Commit
Source Hash
```

例如：

```text
spdlog @ fixed version
jsoncons @ fixed version
SQLite @ fixed version
```

发行构建最好记录：

```text
Dependency Version
Source SHA256
License
Build Option
```

保证几年后仍然能够重现旧版本。

---

# 63. Production / Development / Optional 分离

必须区分：

```text
Production Dependencies

Development Dependencies

Optional Dependencies
```

禁止因为：

```text
test
benchmark
telemetry
experimental feature
```

污染主程序依赖树。

---

# 64. Header-only 使用规则

禁止认为：

> Header-only = 没有成本。

像：

```text
JSON
Units
Template Libraries
```

可能显著增加：

```text
Compile Time
PCH Size
Binary Size
Template Instantiation
```

因此第三方重模板头文件应限制在 Adapter 层。

例如：

```text
jsoncons
```

尽量不要扩散到：

```text
cad/
cam/
runtime/
```

---

# 65. C++ 标准

建议 Kernel 最低：

```text
C++20
```

充分利用：

```text
std::jthread
std::stop_token
std::span
std::chrono
std::filesystem
std::source_location
std::variant
```

优先标准库。

原则：

```text
Standard Library
      ↓
Lightweight Library
      ↓
Large Framework
```

只有前两者无法满足需求时才考虑大型框架。

---

# 66. Kernel 强制架构规则

以下规则建议写进正式 `ARCHITECTURE.md`。

1. 所有业务状态修改必须经过 Transaction。
2. 所有公开业务修改必须定义 Command。
3. 所有纯读取使用 Query。
4. 所有长任务必须进入 TaskRuntime。
5. 跨 Command 流程进入 WorkflowRuntime。
6. 后台任务不得直接修改活动 Document。
7. 后台结果必须经过 Revision 校验。
8. Domain Event 只能在 Transaction Commit 后发布。
9. Event 与 Log 永远分离。
10. GUI 不得直接修改业务对象。
11. CLI / Script / AI 不得绕过 Application API。
12. 跨边界对象必须使用稳定 ID。
13. 禁止裸指针与 OCC index 作为持久身份。
14. Command / Query / Event / Workflow / Project 必须版本化。
15. 外部 Command 支持 Idempotency。
16. 并发修改支持 Revision Preconditions。
17. 长任务必须支持 Cancellation / Progress。
18. Task 并发必须由 Scheduler 管理。
19. 全项目使用统一 Error Model。
20. 所有关键执行链必须具有 TraceId。
21. 模块间禁止隐式循环依赖。
22. Kernel 禁止依赖 CAD/CAM/Machine/OCC/Qt Widgets。
23. 危险硬件能力必须受到 Capability 控制。
24. Headless 与 GUI 使用完全相同的业务能力。
25. GUI 能完成的业务原则上必须可以通过 CLI 重现。

---

# 67. 新增第三方依赖规则

26. 第三方类型禁止进入 Kernel Public API。
27. 第三方库必须通过 Infrastructure Adapter 接入。
28. Domain Module 原则上禁止直接引用 Infrastructure 实现。
29. 一个基础职责原则上只保留一个主要第三方实现。
30. 优先 C++ Standard Library。
31. 小型通用能力优先使用轻量单用途库。
32. 禁止为单一小功能引入大型 Framework。
33. 所有第三方版本必须固定。
34. Production / Development / Optional 依赖必须分离。
35. 所有第三方 Error 必须转换成 Kernel Error。
36. 第三方库替换不得影响 Command / Query / Domain Public API。
37. Header-only 第三方库不得无控制扩散。
38. Kernel 核心产品语义不得委托给第三方 Framework。
39. Infrastructure Adapter 应尽量保持可替换。
40. 所有第三方依赖必须记录许可证和版本来源。

---

# 68. 自研与第三方的最终边界

判断一个功能是否引入第三方：

```text
                  一个能力
                     │
            ┌────────┴────────┐
            │                 │
         通用基础能力        产品语义
            │                 │
            ▼                 ▼
       优先第三方           自己实现
```

例如：

```text
Logging        → Third Party
JSON Parser    → Third Party
Database       → Third Party
Thread Pool    → Third Party
```

而：

```text
Command
Revision
Transaction
Workflow
Task semantics
Event semantics
Document
Capability
Module lifecycle
```

属于 LaserCNC 自己。

---

# 69. 第一阶段推荐依赖基线

Production：

```text
spdlog
toml11
jsoncons
SQLite
BS::thread_pool
```

Host：

```text
CLI11
```

Development：

```text
Catch2
Google Benchmark
```

Optional：

```text
Taskflow
Zstd
OpenTelemetry
BLAKE3/SHA Backend
mp-units
gRPC
```

---

# 70. 正式开发顺序

## Phase 1

```text
Foundation

StrongId
Value
Result
Error
Schema
```

## Phase 2

```text
AppKernel
ServiceRegistry
ModuleRuntime
```

## Phase 3

```text
Infrastructure Base

spdlog adapter
json adapter
toml adapter
sqlite adapter
thread pool adapter
```

这意味着：

> Infrastructure Adapter 应该在 Kernel 很早期就建立。

---

## Phase 4

```text
Document
ObjectRegistry
Revision
Transaction
```

## Phase 5

```text
CommandRuntime
QueryRuntime
EventBus
```

此阶段要求：

```text
Headless
CLI
Logging
Command Execute
Query Execute
CTest
```

全部跑通。

---

## Phase 6

```text
TaskRuntime
Scheduler
Cancellation
Resource Model
```

## Phase 7

```text
Tracing
Metrics
Diagnostics
```

## Phase 8

```text
Snapshot
Journal
SQLite Persistence
Crash Recovery
```

## Phase 9

```text
Workflow Runtime
Script Runtime
```

## Phase 10

```text
CAD vNext
```

## Phase 11

```text
CAM vNext
```

## Phase 12

```text
Qt GUI → Command/Query
```

## Phase 13

```text
External RPC
```

## Phase 14

```text
AI Planner / MCP / Agent
```

---

# 71. 第一阶段最小闭环

最初只实现：

```text
project.new

cad.primitive.box.create
cad.primitive.cylinder.create

cad.object.get
cad.object.list

demo.long_task
```

但同时必须已经拥有：

```text
StrongId
Value
Result/Error

Command
Query
Transaction
Revision
Event

Task
Scheduler

spdlog
JSON
TOML
SQLite
ThreadPool

Headless
CLI
CTest
Trace
```

然后：

```text
lasercnc-cli exec project.new name=test

lasercnc-cli exec cad.primitive.box.create ...

lasercnc-cli query cad.object.list

lasercnc-cli task list

lasercnc-cli log tail

ctest -V
```

全部真实运行。

---

# 72. 最终运行模型

```text
Human → GUI ───────────────┐
Human → CLI ───────────────┤
Script ────────────────────┤
CTest ─────────────────────┤
External System ───────────┤
AI Agent ──────────────────┘
                           ↓
                Command / Query / Workflow
                           ↓
                  Application Kernel
                           ↓
                Domain Modules
          CAD / CAM / Machine / ...
                           ↓
                Infrastructure APIs
                           ↓
     spdlog / SQLite / jsoncons / toml11 / ...
```

---

# 73. 最终架构哲学

整个系统最终形成三层稳定性。

```text
最稳定
────────────────────────────────────
Application Kernel Contracts

Command
Query
Transaction
Revision
Task
Workflow
Event
Value
Error
ID
Capability
Module Lifecycle


稳定但可替换
────────────────────────────────────
Infrastructure Adapters

spdlog
SQLite
jsoncons
toml11
BS::thread_pool


持续变化
────────────────────────────────────
Domain / Product

CAD
CAM
Machine
Collision
Simulation
GUI
AI
```

设计目标是：

> **把变化隔离在领域层；  
> 把长期稳定的业务运行规则沉淀在 Kernel；  
> 把成熟的通用能力交给第三方 Infrastructure；  
> 同时禁止任何第三方 Framework 反过来绑架 Kernel 架构。**

最终 Application Kernel 应成为整个 LaserCNC 软件未来长期发展的稳定基座。
