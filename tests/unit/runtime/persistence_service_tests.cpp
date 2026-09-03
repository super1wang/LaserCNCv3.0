#include <lasercnc/infrastructure/jsoncons_adapter.hpp>
#include <lasercnc/infrastructure/sha256_hash_service.hpp>
#include <lasercnc/infrastructure/filesystem_snapshot_store.hpp>
#include <lasercnc/infrastructure/bs_thread_pool_executor.hpp>
#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>
#include <lasercnc/kernel/app_kernel.hpp>
#include <lasercnc/observability/log_service.hpp>
#include <lasercnc/persistence/persistence_service.hpp>
#include <lasercnc/runtime/command_runtime.hpp>
#include <lasercnc/runtime/transaction_manager.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "kernel_test_module.hpp"
#include "fault_injecting_backend.hpp"
#include "fault_injecting_data_plane.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using namespace lasercnc::foundation;
using namespace lasercnc::infrastructure;
using namespace lasercnc::kernel;
using namespace lasercnc::persistence;
using namespace lasercnc::runtime;
using namespace lasercnc::state;

namespace {

template <typename Id>
Id validId(const char* value)
{
    auto created = Id::create(value);
    if(!created) {
        throw std::logic_error("Invalid test ID");
    }
    return std::move(created).value();
}

std::filesystem::path uniqueDatabasePath()
{
    static std::atomic_ullong sequence {0U};
    return std::filesystem::temp_directory_path()
        / ("lasercnc-persistence-service-"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
           + '-' + std::to_string(sequence.fetch_add(1U)) + ".db");
}

std::filesystem::path uniqueSnapshotDirectory()
{
    static std::atomic_ullong sequence {0U};
    return std::filesystem::temp_directory_path()
        / ("lasercnc-persistence-snapshots-"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
           + '-' + std::to_string(sequence.fetch_add(1U)));
}

TransactionCommit commit(
    const char* transaction,
    RevisionSet before,
    RevisionSet after,
    const char* data)
{
    const auto objectId = validId<ObjectId>("object.persisted");
    return TransactionCommit {
        validId<TransactionId>(transaction),
        validId<ProjectId>("project.persisted"),
        validId<DocumentId>("document.persisted"),
        std::move(before),
        std::move(after),
        std::vector<ObjectChange> {ObjectChange {
            ObjectChangeKind::Created,
            objectId,
            std::nullopt,
            ObjectRecord {
                objectId,
                validId<ObjectTypeId>("kernel.persistence.test"),
                Value {data}}}},
        {}};
}

void removeDatabase(const std::filesystem::path& path)
{
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove(path, ignored));
    static_cast<void>(std::filesystem::remove(path.string() + "-wal", ignored));
    static_cast<void>(std::filesystem::remove(path.string() + "-shm", ignored));
}

void removeSnapshotDirectory(const std::filesystem::path& path)
{
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove_all(path, ignored));
}

void configureService(PersistenceService& service, const std::filesystem::path& path)
{
    auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
    REQUIRE(backend.hasValue());
    REQUIRE(service
                .configure(
                    std::move(backend).value(),
                    std::make_shared<JsonconsAdapter>(),
                    std::make_shared<Sha256HashService>())
                .hasValue());
    REQUIRE(service.initialize().hasValue());
}

void configureService(
    PersistenceService& service,
    const std::filesystem::path& path,
    const std::filesystem::path& snapshotDirectory)
{
    auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
    REQUIRE(backend.hasValue());
    auto snapshots = FilesystemSnapshotStore::create(
        FilesystemSnapshotStoreOptions {snapshotDirectory, 1024U * 1024U});
    REQUIRE(snapshots.hasValue());
    REQUIRE(service
                .configure(
                    std::move(backend).value(),
                    std::make_shared<JsonconsAdapter>(),
                    std::make_shared<Sha256HashService>(),
                    std::move(snapshots).value())
                .hasValue());
    REQUIRE(service.initialize().hasValue());
}

void configureKernelPersistence(
    lasercnc::kernel::AppKernel& kernel,
    const std::filesystem::path& path,
    const std::filesystem::path& snapshotDirectory)
{
    auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
    REQUIRE(backend.hasValue());
    auto snapshots = FilesystemSnapshotStore::create(
        FilesystemSnapshotStoreOptions {snapshotDirectory, 1024U * 1024U});
    REQUIRE(snapshots.hasValue());
    REQUIRE(kernel.persistence()
                .configure(
                    std::move(backend).value(),
                    std::make_shared<JsonconsAdapter>(),
                    std::make_shared<Sha256HashService>(),
                    std::move(snapshots).value())
                .hasValue());
}

class ThrowingBackend final : public lasercnc::platform::IPersistenceBackend {
public:
    Result<std::size_t> execute(std::string_view, std::span<const Value>) override
    {
        throw std::runtime_error("expected backend exception");
    }

    Result<std::vector<lasercnc::platform::PersistenceRow>> query(
        std::string_view,
        std::span<const Value>) override
    {
        return Result<std::vector<lasercnc::platform::PersistenceRow>>::success({});
    }

    Result<void> beginTransaction() override
    {
        ++begins;
        active = true;
        return Result<void>::success();
    }

    Result<void> commitTransaction() override
    {
        active = false;
        return Result<void>::success();
    }

    Result<void> rollbackTransaction() override
    {
        ++rollbacks;
        active = false;
        return Result<void>::success();
    }

    std::size_t begins{0U};
    std::size_t rollbacks{0U};
    bool active{false};
};

class FailingTaskTerminalBackend final
    : public lasercnc::platform::IPersistenceBackend {
public:
    explicit FailingTaskTerminalBackend(
        std::unique_ptr<lasercnc::platform::IPersistenceBackend> delegate)
        : delegate_(std::move(delegate))
    {
    }

    Result<std::size_t> execute(
        std::string_view statement,
        std::span<const Value> parameters = {}) override
    {
        if(failTerminal.load(std::memory_order_acquire)
           && statement.starts_with("UPDATE task_history SET status=")) {
            return Result<std::size_t>::failure(makeError(
                "Test.TaskTerminalPersistFailed",
                ErrorCategory::Infrastructure,
                "expected"));
        }
        return delegate_->execute(statement, parameters);
    }

    Result<std::vector<lasercnc::platform::PersistenceRow>> query(
        std::string_view statement,
        std::span<const Value> parameters = {}) override
    {
        return delegate_->query(statement, parameters);
    }

    Result<void> beginTransaction() override
    {
        return delegate_->beginTransaction();
    }

    Result<void> commitTransaction() override
    {
        return delegate_->commitTransaction();
    }

    Result<void> rollbackTransaction() override
    {
        return delegate_->rollbackTransaction();
    }

    std::atomic_bool failTerminal{false};

private:
    std::unique_ptr<lasercnc::platform::IPersistenceBackend> delegate_;
};

class FailingWorkflowCheckpointBackend final
    : public lasercnc::platform::IPersistenceBackend {
public:
    explicit FailingWorkflowCheckpointBackend(
        std::unique_ptr<lasercnc::platform::IPersistenceBackend> delegate)
        : delegate_(std::move(delegate))
    {
    }

    Result<std::size_t> execute(
        std::string_view statement,
        std::span<const Value> parameters = {}) override
    {
        if(failCheckpoint.load(std::memory_order_acquire)
           && statement.starts_with("INSERT INTO workflow_instances")) {
            return Result<std::size_t>::failure(makeError(
                "Test.WorkflowCheckpointFailed",
                ErrorCategory::Infrastructure,
                "expected workflow checkpoint failure"));
        }
        return delegate_->execute(statement, parameters);
    }

    Result<std::vector<lasercnc::platform::PersistenceRow>> query(
        std::string_view statement,
        std::span<const Value> parameters = {}) override
    {
        return delegate_->query(statement, parameters);
    }

    Result<void> beginTransaction() override
    {
        return delegate_->beginTransaction();
    }

    Result<void> commitTransaction() override
    {
        return delegate_->commitTransaction();
    }

    Result<void> rollbackTransaction() override
    {
        return delegate_->rollbackTransaction();
    }

    std::atomic_bool failCheckpoint{false};

private:
    std::unique_ptr<lasercnc::platform::IPersistenceBackend> delegate_;
};

class ReentrantSerializer final : public IValueSerializer {
public:
    ReentrantSerializer(DocumentStore& documents, DocumentId documentId)
        : documents_(documents), documentId_(std::move(documentId))
    {
    }

    Result<std::string> serialize(const Value& value) const override
    {
        auto snapshot = documents_.snapshot(documentId_);
        sawOldSnapshot = snapshot.hasValue()
            && snapshot.value().revisions().at(RevisionScope::Document) == Revision {0U};
        return json_.serialize(value);
    }

    Result<Value> deserialize(std::string_view payload) const override
    {
        return json_.deserialize(payload);
    }

    mutable bool sawOldSnapshot{false};

private:
    DocumentStore& documents_;
    DocumentId documentId_;
    JsonconsAdapter json_;
};

class FailingSerializer final : public IValueSerializer {
public:
    Result<std::string> serialize(const Value&) const override
    {
        return Result<std::string>::failure(makeError(
            "Test.SerializeFailed", ErrorCategory::Infrastructure, "expected"));
    }

    Result<Value> deserialize(std::string_view) const override
    {
        return Result<Value>::failure(makeError(
            "Test.DeserializeFailed", ErrorCategory::Infrastructure, "expected"));
    }
};

class NullLogService final : public lasercnc::observability::ILogService {
public:
    Result<void> write(const lasercnc::observability::LogRecord&) override
    {
        return Result<void>::success();
    }

    Result<void> flush() override { return Result<void>::success(); }
};

class PersistentCreateHandler final : public ICommandHandler {
public:
    Result<Value> execute(
        const CommandRequest& request,
        ApplicationTransaction& transaction) override
    {
        ++calls;
        const auto* arguments = request.arguments.getIf<Value::Object>();
        const auto* idValue = arguments == nullptr
            ? nullptr
            : arguments->at("id").getIf<std::string>();
        const auto* dataValue = arguments == nullptr
            ? nullptr
            : arguments->at("data").getIf<std::string>();
        if(idValue == nullptr || dataValue == nullptr) {
            return Result<Value>::failure(makeError(
                "Test.InvalidArguments",
                ErrorCategory::Validation,
                "Invalid persistent command arguments"));
        }
        auto objectId = ObjectId::create(*idValue);
        if(!objectId) {
            return Result<Value>::failure(std::move(objectId).error());
        }
        const auto stableObjectId = objectId.value();
        auto created = transaction.createObject(ObjectRecord {
            std::move(objectId).value(),
            validId<ObjectTypeId>("kernel.persistence.command"),
            Value {*dataValue}, schemaVersion});
        if(!created) {
            return Result<Value>::failure(std::move(created).error());
        }
        auto event = transaction.collectEvent(lasercnc::messaging::PendingDomainEvent {
            validId<EventName>("kernel.persistence.command-created"),
            Version {1U, 0U, 0U},
            stableObjectId,
            Value {*idValue}});
        if(!event) {
            return Result<Value>::failure(std::move(event).error());
        }
        return Result<Value>::success(Value {Value::Object {
            {"id", Value {*idValue}}, {"data", Value {*dataValue}}}});
    }

    Version schemaVersion{1U, 0U, 0U};
    std::size_t calls{0U};
};

class PersistentTaskHandler final : public ITaskHandler {
public:
    Result<Value> execute(const TaskRequest&, const TaskContext&) override
    {
        ++calls;
        return Result<Value>::success(Value {Value::Object {
            {"value", Value {"completed"}}}});
    }

    std::atomic_size_t calls{0U};
};

class BlockingDocumentTaskHandler final : public ITaskHandler {
public:
    BlockingDocumentTaskHandler(
        std::promise<void>& enteredPromise,
        std::shared_future<void> releaseFuture)
        : entered_(enteredPromise), release_(std::move(releaseFuture))
    {
    }

    Result<Value> execute(const TaskRequest&, const TaskContext&) override
    {
        entered_.set_value();
        release_.wait();
        return Result<Value>::success(Value {Value::Object {
            {"value", Value {"completed"}}}});
    }

private:
    std::promise<void>& entered_;
    std::shared_future<void> release_;
};

class PersistentAsyncHandler final : public IAsyncCommandHandler {
public:
    Result<AsyncCommandPlan> prepare(const CommandRequest& command) override
    {
        ++calls;
        auto taskId = TaskId::create(
            "task." + std::string(command.requestId.value()));
        if(!taskId) {
            return Result<AsyncCommandPlan>::failure(std::move(taskId).error());
        }
        AsyncCommandPlan plan {
            TaskRequest {
                std::move(taskId).value(),
                validId<TaskName>("kernel.persistence.async-task"),
                Value {Value::Object {{"input", Value {"durable"}}}},
                command.traceId},
            Value {Value::Object {{"accepted", Value {true}}}}};
        plan.task.resources = resources;
        return Result<AsyncCommandPlan>::success(std::move(plan));
    }

    std::atomic_size_t calls{0U};
    std::vector<ResourceClaim> resources;
};

class TestEffectHandler final : public IExternalEffectHandler {
public:
    Result<Value> execute(
        const CommandRequest&,
        const ExternalEffectContext& context) override
    {
        ++calls;
        resumed.store(context.resumed, std::memory_order_release);
        if(fail.load(std::memory_order_acquire)) {
            return Result<Value>::failure(makeError(
                "Test.ExternalEffectFailed",
                ErrorCategory::Infrastructure,
                "expected external-effect failure"));
        }
        return Result<Value>::success(Value {Value::Object {
            {"published", Value {true}}}});
    }

    std::atomic_size_t calls{0U};
    std::atomic_bool fail{false};
    std::atomic_bool resumed{false};
};

class TestEffectGuard final : public IEffectGuard {
public:
    Result<void> evaluate(
        const CommandRequest&,
        const CommandDescriptor&,
        const EffectGuardContext&) override
    {
        ++calls;
        if(!allow.load(std::memory_order_acquire)) {
            return Result<void>::failure(makeError(
                "Test.EffectGuardDenied",
                ErrorCategory::Authorization,
                "expected effect guard denial"));
        }
        return Result<void>::success();
    }

    std::atomic_size_t calls{0U};
    std::atomic_bool allow{true};
};

class BlockingEffectHandler final : public IExternalEffectHandler {
public:
    Result<Value> execute(const CommandRequest&, const ExternalEffectContext&) override
    {
        std::unique_lock lock(mutex_);
        ++calls;
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&]() { return released_; });
        return Result<Value>::success(Value {Value::Object {
            {"published", Value {true}}}});
    }

    void waitUntilEntered()
    {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&]() { return entered_; });
    }

    void release()
    {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }

    std::atomic_size_t calls{0U};

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_{false};
    bool released_{false};
};

class FixedDiagnosticCheck final
    : public lasercnc::observability::IDiagnosticCheck {
public:
    explicit FixedDiagnosticCheck(DiagnosticId id) : id_(std::move(id)) {}

    Result<lasercnc::observability::DiagnosticReport> run() override
    {
        ++calls;
        return Result<lasercnc::observability::DiagnosticReport>::success(
            lasercnc::observability::DiagnosticReport {
                id_,
                lasercnc::observability::DiagnosticStatus::Degraded,
                "maintenance window",
                Value {Value::Object {{"attempt", Value {std::to_string(calls)}}}},
                {}});
    }

    std::size_t calls{0U};

private:
    DiagnosticId id_;
};

Schema objectSchema(const char* id)
{
    auto created = Schema::create(
        validId<SchemaId>(id), Version {1U, 0U, 0U}, SchemaKind::Object);
    if(!created) {
        throw std::logic_error("Invalid test schema");
    }
    return std::move(created).value();
}

CommandDescriptor persistentCommandDescriptor()
{
    return CommandDescriptor {
        validId<CommandName>("kernel.persistence.create"),
        Version {1U, 0U, 0U},
        objectSchema("schema.persistence.command.arguments"),
        objectSchema("schema.persistence.command.result"),
        ExecutionMode::Synchronous,
        SideEffectLevel::DocumentWrite,
        validId<CapabilityId>("document.write"),
        false,
        true,
        true};
}

CommandDescriptor persistentAsyncCommandDescriptor()
{
    return CommandDescriptor {
        validId<CommandName>("kernel.persistence.async-command"),
        Version {1U, 0U, 0U},
        objectSchema("schema.persistence.async-command.arguments"),
        objectSchema("schema.persistence.async-command.result"),
        ExecutionMode::Asynchronous,
        SideEffectLevel::ReadOnly,
        validId<CapabilityId>("task.submit"),
        false,
        true,
        true};
}

CommandDescriptor externalEffectDescriptor(
    const char* name,
    ReplayPolicy replayPolicy)
{
    auto descriptor = CommandDescriptor {
        validId<CommandName>(name),
        Version {1U, 0U, 0U},
        objectSchema("schema.persistence.effect.arguments"),
        objectSchema("schema.persistence.effect.result"),
        ExecutionMode::Synchronous,
        SideEffectLevel::Publish,
        validId<CapabilityId>("effect.publish"),
        false,
        false,
        true};
    descriptor.scope = ExecutionScope::Global;
    descriptor.replayPolicy = replayPolicy;
    descriptor.effectGuards = {validId<EffectGuardId>("guard.effect.publish")};
    descriptor.resources = {ResourceClaim {
        ResourceKind::DiskIO,
        validId<ResourceId>("resource.effect.publish"),
        ResourceAccess::Exclusive,
        1U}};
    return descriptor;
}

CommandRequest externalEffectRequest(
    const char* requestId,
    const char* command,
    const SessionId& sessionId,
    const IdempotencyKey& key)
{
    return CommandRequest {
        validId<RequestId>(requestId),
        ExecutionContext {sessionId, std::nullopt, std::nullopt},
        validId<CommandName>(command),
        Version {1U, 0U, 0U},
        Value {Value::Object {{"target", Value {"artifact.test"}}}},
        std::nullopt,
        validId<CorrelationId>("correlation.effect"),
        validId<TraceId>("trace.effect"),
        key};
}

TaskDescriptor persistentTaskDescriptor()
{
    return TaskDescriptor {
        validId<TaskName>("kernel.persistence.async-task"),
        Version {1U, 0U, 0U},
        objectSchema("schema.persistence.async-task.input"),
        objectSchema("schema.persistence.async-task.result")};
}

WorkflowDefinition persistentWorkflowDefinition(bool commandStep)
{
    WorkflowStep step {
        validId<WorkflowStepId>("step.persistence"),
        commandStep ? WorkflowStepKind::Command : WorkflowStepKind::Assign,
        {},
        std::nullopt,
        std::nullopt,
        std::nullopt,
        commandStep
            ? Value {}
            : Value {Value::Object {{"checkpoint", Value {true}}}},
        {},
        commandStep ? "commandResult" : "assigned",
        std::nullopt,
        WorkflowRetryPolicy {},
        std::nullopt};
    if(commandStep) {
        step.command = WorkflowCommandCall {
            validId<CommandName>("kernel.persistence.create"),
            Version {1U, 0U, 0U},
            Value {Value::Object {
                {"data", Value {"workflow"}},
                {"id", Value {"object.workflow-recovered"}},
            }}};
    }
    return WorkflowDefinition {
        WorkflowDescriptor {
            validId<WorkflowName>("workflow.persistence.test"),
            Version {1U, 0U, 0U},
            objectSchema("schema.persistence.workflow.input"),
            objectSchema("schema.persistence.workflow.result")},
        {std::move(step)},
        Value {Value::Object {}}};
}

WorkflowRequest persistentWorkflowRequest(const char* id)
{
    return WorkflowRequest {
        validId<WorkflowId>(id),
        validId<WorkflowName>("workflow.persistence.test"),
        Value {Value::Object {}},
        validId<SessionId>("session.workflow-persistence"),
        validId<ProjectId>("project.workflow-persistence"),
        validId<DocumentId>("document.workflow-persistence"),
        validId<CorrelationId>("correlation.workflow-persistence"),
        validId<TraceId>("trace.workflow-persistence"),
        std::nullopt,
        std::nullopt};
}

WorkflowSnapshot persistentWorkflowSnapshot(
    const WorkflowRequest& request,
    WorkflowState workflowState,
    WorkflowStepState stepState,
    std::uint32_t attempt = 0U)
{
    return WorkflowSnapshot {
        request.workflowId,
        request.workflow,
        Version {1U, 0U, 0U},
        workflowState,
        request.input,
        {WorkflowStepSnapshot {
            validId<WorkflowStepId>("step.persistence"),
            stepState,
            attempt,
            std::nullopt,
            stepState == WorkflowStepState::Running
                ? std::optional {std::chrono::system_clock::now()}
                : std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt}},
        std::nullopt,
        std::nullopt,
        {}};
}

CommandRequest persistentCommandRequest(
    const char* requestId,
    const ProjectId& projectId,
    const DocumentId& documentId,
    const SessionId& sessionId,
    const IdempotencyKey& key,
    const char* objectId)
{
    return CommandRequest {
        validId<RequestId>(requestId),
        ExecutionContext {sessionId, projectId, documentId},
        validId<CommandName>("kernel.persistence.create"),
        Version {1U, 0U, 0U},
        Value {Value::Object {
            {"data", Value {"durable"}}, {"id", Value {objectId}}}},
        std::nullopt,
        validId<CorrelationId>("correlation.persistence"),
        validId<TraceId>("trace.persistence"),
        key,
        std::nullopt};
}

void configureRuntimeKernel(
    lasercnc::kernel::AppKernel& kernel,
    const std::filesystem::path& database,
    const std::filesystem::path& snapshotDirectory,
    const SessionId& sessionId,
    std::shared_ptr<PersistentCreateHandler> handler,
    ObjectPersistencePolicy policy = ObjectPersistencePolicy::Durable)
{
    auto objectType = lasercnc::test::valueObjectType("kernel.persistence.command");
    objectType.descriptor.persistencePolicy = policy;
    REQUIRE(lasercnc::test::registerObjectType(kernel, std::move(objectType)).hasValue());
    auto adapter = std::make_shared<JsonconsAdapter>();
    REQUIRE(kernel.executionServices()
                .configure(adapter, std::make_shared<NullLogService>())
                .hasValue());
    const std::array capabilities {validId<CapabilityId>("document.write")};
    REQUIRE(kernel.capabilities().replace(sessionId, capabilities).hasValue());
    REQUIRE(lasercnc::test::registerCommand(kernel, persistentCommandDescriptor(), std::move(handler))
                .hasValue());
    auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {database});
    REQUIRE(backend.hasValue());
    auto snapshots = FilesystemSnapshotStore::create(
        FilesystemSnapshotStoreOptions {snapshotDirectory, 1024U * 1024U});
    REQUIRE(snapshots.hasValue());
    REQUIRE(kernel.persistence()
                .configure(
                    std::move(backend).value(),
                    adapter,
                    std::make_shared<Sha256HashService>(),
                    std::move(snapshots).value())
                .hasValue());
}

void configureAsyncRuntimeKernel(
    lasercnc::kernel::AppKernel& kernel,
    const std::filesystem::path& database,
    const SessionId& sessionId,
    std::shared_ptr<PersistentAsyncHandler> commandHandler,
    std::shared_ptr<PersistentTaskHandler> taskHandler,
    std::unique_ptr<lasercnc::platform::IPersistenceBackend> injectedBackend = nullptr,
    std::unique_ptr<lasercnc::platform::ITaskExecutor> injectedExecutor = nullptr)
{
    auto adapter = std::make_shared<JsonconsAdapter>();
    REQUIRE(kernel.executionServices()
                .configure(adapter, std::make_shared<NullLogService>())
                .hasValue());
    const std::array capabilities {validId<CapabilityId>("task.submit")};
    REQUIRE(kernel.capabilities().replace(sessionId, capabilities).hasValue());
    REQUIRE(lasercnc::test::registerAsyncCommand(kernel,
                    persistentAsyncCommandDescriptor(), std::move(commandHandler))
                .hasValue());
    REQUIRE(lasercnc::test::registerTask(kernel, persistentTaskDescriptor(), std::move(taskHandler))
                .hasValue());
    if(injectedBackend == nullptr) {
        auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {database});
        REQUIRE(backend.hasValue());
        injectedBackend = std::move(backend).value();
    }
    REQUIRE(kernel.persistence()
                .configure(
                    std::move(injectedBackend),
                    adapter,
                    std::make_shared<Sha256HashService>())
                .hasValue());
    if(injectedExecutor == nullptr) {
        auto executor = BsThreadPoolExecutor::create(BsThreadPoolExecutorOptions {1U});
        REQUIRE(executor.hasValue());
        injectedExecutor = std::move(executor).value();
    }
    REQUIRE(kernel.configureTaskExecutor(std::move(injectedExecutor)).hasValue());
}

CommandRequest persistentAsyncCommandRequest(
    const char* requestId,
    const ProjectId& projectId,
    const DocumentId& documentId,
    const SessionId& sessionId,
    const IdempotencyKey& key)
{
    return CommandRequest {
        validId<RequestId>(requestId),
        ExecutionContext {sessionId, projectId, documentId},
        validId<CommandName>("kernel.persistence.async-command"),
        Version {1U, 0U, 0U},
        Value {Value::Object {{"input", Value {"durable"}}}},
        std::nullopt,
        validId<CorrelationId>("correlation.persistence.async"),
        validId<TraceId>("trace.persistence.async"),
        key,
        std::nullopt};
}

} // namespace

TEST_CASE("Document open refuses unsupported durable object versions and remains detached", "[persistence][object-type][admission]")
{
    const auto path = uniqueDatabasePath();
    const auto snapshots = uniqueSnapshotDirectory();
    const auto project = validId<ProjectId>("project.open-admission");
    const auto document = validId<DocumentId>("document.open-admission");
    const auto session = validId<SessionId>("session.open-admission");
    {
        AppKernel kernel;
        configureRuntimeKernel(kernel, path, snapshots, session, std::make_shared<PersistentCreateHandler>());
        REQUIRE(kernel.addDocument(project, document).hasValue());
        REQUIRE(kernel.bootstrap().hasValue());
        auto created = kernel.execution().executeCommand(persistentCommandRequest(
            "request.open-admission", project, document, session,
            validId<IdempotencyKey>("key.open-admission"), "object.open-admission"));
        REQUIRE(created.hasValue());
        REQUIRE(created.value().commit.has_value());
        const auto& first = *created.value().commit;
        REQUIRE(kernel.documentRuntime().close(document).hasValue());
        auto after = *first.changes.front().after;
        after.schemaVersion = Version {9U, 0U, 0U};
        auto revisions = RevisionManager::advance(first.revisionsAfter,
            std::array{RevisionScope::Project, RevisionScope::Document});
        REQUIRE(revisions.hasValue());
        // Inject well-formed but unsupported durable material through the low-level adapter.
        // 中文翻译：通过底层持久化适配器注入编码正确但版本未注册的恢复材料。
        TransactionCommit injected {validId<TransactionId>("tx.open-admission.injected"),
            project, document, first.revisionsAfter, revisions.value(),
            {{ObjectChangeKind::Updated, after.id, first.changes.front().after, after}}, {}};
        REQUIRE(kernel.persistence().append(injected).hasValue());
        auto opened = kernel.documentRuntime().open(document);
        REQUIRE_FALSE(opened.hasValue());
        CHECK(std::string(opened.error().code.value()) == "ObjectType.UnsupportedVersion");
        CHECK_FALSE(kernel.documents().contains(document));
        CHECK(kernel.documentRuntime().lifecycle(document).value().state == DocumentLifecycleState::Detached);
        CHECK(kernel.persistence().documentCatalog().value().front().state == DocumentPersistenceState::Detached);
        REQUIRE(kernel.shutdown().hasValue());
    }
    removeDatabase(path);
    removeSnapshotDirectory(snapshots);
}

TEST_CASE("Object admission failure leaves no journal history event or completed idempotency", "[persistence][object-type][admission]")
{
    const auto path = uniqueDatabasePath();
    const auto snapshots = uniqueSnapshotDirectory();
    const auto project = validId<ProjectId>("project.admission");
    const auto document = validId<DocumentId>("document.admission");
    const auto session = validId<SessionId>("session.admission");
    bool transient = false;
    SECTION("unsupported schema leaves no durable result and permits retry after restart") {}
    SECTION("transient type cannot become durable") { transient = true; }
    {
        AppKernel kernel;
        auto handler = std::make_shared<PersistentCreateHandler>();
        handler->schemaVersion = transient ? Version {1U, 0U, 0U} : Version {9U, 0U, 0U};
        configureRuntimeKernel(kernel, path, snapshots, session, handler,
            transient ? ObjectPersistencePolicy::Transient : ObjectPersistencePolicy::Durable);
        REQUIRE(kernel.addDocument(project, document).hasValue());
        std::size_t events = 0U;
        auto subscription = kernel.events().subscribe(
            validId<SubscriptionId>("subscription.admission"),
            lasercnc::messaging::EventFilter{lasercnc::messaging::EventKind::Domain, std::nullopt},
            lasercnc::messaging::DeliveryMode::Immediate,
            [&](const auto&) { ++events; });
        REQUIRE(subscription.hasValue());
        REQUIRE(kernel.bootstrap().hasValue());
        const auto request = persistentCommandRequest("request.admission", project, document, session,
            validId<IdempotencyKey>("key.admission"), "object.admission");
        auto rejected = kernel.execution().executeCommand(request);
        REQUIRE_FALSE(rejected.hasValue());
        CHECK(std::string(rejected.error().code.value()) ==
            (transient ? "ObjectType.TransientPersistenceDenied" : "ObjectType.UnsupportedVersion"));
        CHECK(kernel.documents().snapshot(document).value().objects().empty());
        CHECK(kernel.documents().snapshot(document).value().revisions() == RevisionSet{});
        CHECK(kernel.persistence().journalAfter(document, 0U).value().empty());
        CHECK(kernel.history().snapshot(document).value().entries.empty());
        CHECK(events == 0U);
        if(!transient) {
            handler->schemaVersion = Version {1U, 0U, 0U};
            CHECK_FALSE(kernel.execution().executeCommand(request).hasValue());
            CHECK(handler->calls == 1U); // The existing in-process failure cache is preserved.
            // 中文翻译：保留既有的进程内失败结果缓存契约。
        } else {
            const auto other = validId<DocumentId>("document.transient.attach");
            DocumentImage image{project, other, RevisionSet{}, {{validId<ObjectId>("object.transient"),
                validId<ObjectTypeId>("kernel.persistence.command"), Value {"data"}}}};
            CHECK_FALSE(kernel.documentRuntime().attach(image).hasValue());
            CHECK_FALSE(kernel.documents().contains(other));
            CHECK(kernel.persistence().documentCatalog().value().size() == 1U);
        }
        REQUIRE(kernel.shutdown().hasValue());
    }
    if(!transient) {
        AppKernel kernel;
        auto handler = std::make_shared<PersistentCreateHandler>();
        configureRuntimeKernel(kernel, path, snapshots, session, handler);
        REQUIRE(kernel.bootstrap().hasValue());
        auto accepted = kernel.execution().executeCommand(persistentCommandRequest(
            "request.admission.retry", project, document, session,
            validId<IdempotencyKey>("key.admission"), "object.admission"));
        REQUIRE(accepted.hasValue());
        CHECK_FALSE(accepted.value().replayed);
        CHECK(handler->calls == 1U);
        CHECK(kernel.persistence().journalAfter(document, 0U).value().size() == 1U);
        REQUIRE(kernel.shutdown().hasValue());
    }
    removeDatabase(path);
    removeSnapshotDirectory(snapshots);
}

TEST_CASE("Persistence preserves object schema versions across journal snapshot and idempotency", "[persistence][object-type]")
{
    const auto path = uniqueDatabasePath();
    const auto snapshotDirectory = uniqueSnapshotDirectory();
    const auto project = validId<ProjectId>("project.persisted");
    const auto document = validId<DocumentId>("document.persisted");
    const auto key = validId<IdempotencyKey>("key.schema");
    const Value signature {"signature.schema"};
    const RevisionSet one {Revision {1U}, Revision {1U}, Revision {1U}, Revision {}, Revision {}, Revision {}};
    const RevisionSet two {Revision {2U}, Revision {2U}, Revision {2U}, Revision {}, Revision {}, Revision {}};
    auto first = commit("tx.schema.1", RevisionSet {}, one, "schema-data");
    first.changes.front().after->schemaVersion = Version {2U, 7U, 11U};
    {
        PersistenceService service;
        configureService(service, path, snapshotDirectory);
        REQUIRE(service.claimCommand(key, signature).hasValue());
        REQUIRE(service.append(first, TransactionIdempotency{key, signature, Value {"result"}}).hasValue());
        auto recovered = service.recover();
        REQUIRE(recovered.hasValue());
        REQUIRE(recovered.value().documents.size() == 1U);
        CHECK(recovered.value().documents.front().objects.front().schemaVersion == Version {2U, 7U, 11U});
        DocumentStore documents;
        REQUIRE(documents.addDocument(project, document).hasValue());
        TransactionManager transactions(documents);
        auto seed = transactions.begin(validId<TransactionId>("tx.schema.snapshot"), document);
        REQUIRE(seed.hasValue());
        REQUIRE(seed.value()->createObject(*first.changes.front().after).hasValue());
        REQUIRE(seed.value()->touchRevision(RevisionScope::Geometry).hasValue());
        REQUIRE(seed.value()->commit().hasValue());
        REQUIRE(service.captureSnapshot(validId<SnapshotId>("snapshot.schema"), documents.snapshot(document).value()).hasValue());
        auto second = commit("tx.schema.2", one, two, "migrated-data");
        auto& change = second.changes.front();
        change.kind = ObjectChangeKind::Updated;
        change.before = first.changes.front().after;
        change.after->schemaVersion = Version {3U, 0U, 2U};
        REQUIRE(service.append(second).hasValue());
    }
    {
        PersistenceService service;
        configureService(service, path, snapshotDirectory);
        auto recovered = service.recover();
        REQUIRE(recovered.hasValue());
        CHECK(recovered.value().journalRecordsReplayed == 1U);
        const auto& image = recovered.value().documents.front();
        CHECK(image.projectId == project);
        CHECK(image.objects.front().schemaVersion == Version {3U, 0U, 2U});
        auto replay = service.claimCommand(key, signature);
        REQUIRE(replay.hasValue());
        REQUIRE(replay.value().replay.has_value());
        REQUIRE(replay.value().replay->commit.has_value());
        CHECK(replay.value().replay->commit->changes.front().after->schemaVersion == Version {2U, 7U, 11U});
    }
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
}

TEST_CASE("PersistenceService migrates and appends an idempotent state journal", "[persistence][journal]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    const RevisionSet zero;
    const RevisionSet one {
        Revision {1U}, Revision {1U}, Revision {1U}, Revision {}, Revision {}, Revision {}};
    const RevisionSet two {
        Revision {2U}, Revision {2U}, Revision {2U}, Revision {}, Revision {}, Revision {}};

    {
        PersistenceService service;
        CHECK_FALSE(service.initialize().hasValue());
        configureService(service, path);
        CHECK(service.configured());
        CHECK(service.ready());
        CHECK(service.initialize().hasValue());

        auto first = service.append(commit("transaction.persisted.1", zero, one, "one"));
        REQUIRE(first.hasValue());
        CHECK(first.value().sequence == 1U);
        CHECK(std::string(first.value().digest.value()).starts_with("sha256:"));

        auto replay = service.append(commit("transaction.persisted.1", zero, one, "one"));
        REQUIRE(replay.hasValue());
        CHECK(replay.value().sequence == first.value().sequence);
        CHECK(replay.value().digest == first.value().digest);

        auto conflict = service.append(commit("transaction.persisted.1", zero, one, "changed"));
        REQUIRE_FALSE(conflict.hasValue());
        CHECK(std::string(conflict.error().code.value())
              == "Persistence.JournalTransactionConflict");

        auto second = service.append(commit("transaction.persisted.2", one, two, "two"));
        REQUIRE(second.hasValue());
        CHECK(second.value().sequence == 2U);
        auto records = service.journalAfter(validId<DocumentId>("document.persisted"), 0U);
        REQUIRE(records.hasValue());
        REQUIRE(records.value().size() == 2U);
        CHECK(records.value()[0].sequence == 1U);
        CHECK(records.value()[1].sequence == 2U);
        CHECK(service.journalAfter(
            validId<DocumentId>("document.persisted"), 1U).value().size() == 1U);
    }

    {
        PersistenceService reopened;
        configureService(reopened, path);
        auto records = reopened.journalAfter(
            validId<DocumentId>("document.persisted"), 0U);
        REQUIRE(records.hasValue());
        CHECK(records.value().size() == 2U);
    }
    removeDatabase(path);
}

TEST_CASE("PersistenceService fails closed on journal corruption", "[persistence][journal][recovery]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    {
        PersistenceService service;
        configureService(service, path);
        const RevisionSet one {
            Revision {1U}, Revision {1U}, Revision {1U}, Revision {}, Revision {}, Revision {}};
        auto appended = service.append(
            commit("transaction.corrupt", RevisionSet {}, one, "safe"));
        REQUIRE(appended.hasValue());

        auto tamper = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(tamper.hasValue());
        const std::array parameters {
            Value {"tampered"}, Value {"transaction.corrupt"}};
        REQUIRE(tamper.value()
                    ->execute(
                        "UPDATE state_journal SET payload=? WHERE transaction_id=?",
                        parameters)
                    .hasValue());

        auto records = service.journalAfter(
            validId<DocumentId>("document.persisted"), 0U);
        REQUIRE_FALSE(records.hasValue());
        CHECK(std::string(records.error().code.value())
              == "Persistence.JournalDigestMismatch");

        const std::array metadataParameters {
            Value {appended.value().payload},
            Value {"project.tampered"},
            Value {"transaction.corrupt"}};
        REQUIRE(tamper.value()
                    ->execute(
                        "UPDATE state_journal SET payload=?,project_id=? "
                        "WHERE transaction_id=?",
                        metadataParameters)
                    .hasValue());
        auto metadataMismatch = service.journalAfter(
            validId<DocumentId>("document.persisted"), 0U);
        REQUIRE_FALSE(metadataMismatch.hasValue());
        CHECK(std::string(metadataMismatch.error().code.value())
              == "Persistence.JournalMetadataMismatch");
    }
    removeDatabase(path);
}

TEST_CASE("PersistenceService captures immutable snapshots aligned with the journal", "[persistence][snapshot]")
{
    const auto path = uniqueDatabasePath();
    const auto snapshotDirectory = uniqueSnapshotDirectory();
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
    const auto projectId = validId<ProjectId>("project.snapshot");
    const auto documentId = validId<DocumentId>("document.snapshot");
    const auto objectId = validId<ObjectId>("object.snapshot");
    const auto firstSnapshotId = validId<SnapshotId>("snapshot.capture-1");
    const auto secondSnapshotId = validId<SnapshotId>("snapshot.capture-2");

    {
        DocumentStore documents;
        REQUIRE(documents.addDocument(projectId, documentId).hasValue());
        PersistenceService persistence;
        configureService(persistence, path, snapshotDirectory);
        TransactionManager transactions(documents, &persistence);

        auto firstTransaction = transactions.begin(
            validId<TransactionId>("transaction.snapshot.1"), documentId);
        REQUIRE(firstTransaction.hasValue());
        REQUIRE(firstTransaction.value()
                    ->createObject(ObjectRecord {
                        objectId,
                        validId<ObjectTypeId>("kernel.persistence.snapshot"),
                        Value {"first"}})
                    .hasValue());
        REQUIRE(firstTransaction.value()->touchRevision(RevisionScope::Geometry).hasValue());
        REQUIRE(firstTransaction.value()->commit().hasValue());

        auto document = documents.snapshot(documentId);
        REQUIRE(document.hasValue());
        auto first = persistence.captureSnapshot(firstSnapshotId, document.value());
        REQUIRE(first.hasValue());
        CHECK(first.value().journalSequence == 1U);
        CHECK(first.value().revisions == document.value().revisions());
        CHECK(std::string(first.value().digest.value()).starts_with("sha256:"));
        CHECK(std::filesystem::is_regular_file(
            snapshotDirectory / "snapshot.capture-1.snapshot"));

        auto repeated = persistence.captureSnapshot(firstSnapshotId, document.value());
        REQUIRE(repeated.hasValue());
        CHECK(repeated.value().payload == first.value().payload);
        CHECK(repeated.value().digest == first.value().digest);
        auto latest = persistence.latestSnapshot(documentId);
        REQUIRE(latest.hasValue());
        REQUIRE(latest.value().has_value());
        CHECK(latest.value()->snapshotId == firstSnapshotId);

        auto secondTransaction = transactions.begin(
            validId<TransactionId>("transaction.snapshot.2"), documentId);
        REQUIRE(secondTransaction.hasValue());
        REQUIRE(secondTransaction.value()->replaceObjectData(objectId, Value {"second"}).hasValue());
        REQUIRE(secondTransaction.value()->commit().hasValue());
        document = documents.snapshot(documentId);
        REQUIRE(document.hasValue());

        auto identityConflict = persistence.captureSnapshot(
            firstSnapshotId, document.value());
        REQUIRE_FALSE(identityConflict.hasValue());
        CHECK(std::string(identityConflict.error().code.value())
              == "Persistence.SnapshotIdentityConflict");
        auto second = persistence.captureSnapshot(secondSnapshotId, document.value());
        REQUIRE(second.hasValue());
        CHECK(second.value().journalSequence == 2U);
    }

    {
        PersistenceService reopened;
        configureService(reopened, path, snapshotDirectory);
        auto latest = reopened.latestSnapshot(documentId);
        REQUIRE(latest.hasValue());
        REQUIRE(latest.value().has_value());
        CHECK(latest.value()->snapshotId == secondSnapshotId);
        CHECK(latest.value()->journalSequence == 2U);

        std::ofstream tampered(
            snapshotDirectory / "snapshot.capture-2.snapshot",
            std::ios::binary | std::ios::trunc);
        REQUIRE(tampered.good());
        tampered << "tampered";
        tampered.close();
        auto rejected = reopened.latestSnapshot(documentId);
        REQUIRE_FALSE(rejected.hasValue());
        CHECK((std::string(rejected.error().code.value())
                   == "Persistence.SnapshotSizeMismatch"
               || std::string(rejected.error().code.value())
                   == "Persistence.SnapshotDigestMismatch"));
        auto recovery = reopened.recover();
        REQUIRE_FALSE(recovery.hasValue());
        CHECK((std::string(recovery.error().code.value())
                   == "Persistence.SnapshotSizeMismatch"
               || std::string(recovery.error().code.value())
                   == "Persistence.SnapshotDigestMismatch"));
    }
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
}

TEST_CASE("PersistenceService rejects snapshots ahead of the journal", "[persistence][snapshot][recovery]")
{
    const auto path = uniqueDatabasePath();
    const auto snapshotDirectory = uniqueSnapshotDirectory();
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
    const auto projectId = validId<ProjectId>("project.snapshot-boundary");
    const auto emptyDocumentId = validId<DocumentId>("document.snapshot-empty");
    const auto advancedDocumentId = validId<DocumentId>("document.snapshot-ahead");

    {
        PersistenceService persistence;
        configureService(persistence, path, snapshotDirectory);
        DocumentStore documents;
        REQUIRE(documents.addDocument(projectId, emptyDocumentId).hasValue());
        REQUIRE(documents.addDocument(projectId, advancedDocumentId).hasValue());

        auto empty = documents.snapshot(emptyDocumentId);
        REQUIRE(empty.hasValue());
        auto captured = persistence.captureSnapshot(
            validId<SnapshotId>("snapshot.empty"), empty.value());
        REQUIRE(captured.hasValue());
        CHECK(captured.value().journalSequence == 0U);

        TransactionManager memoryOnly(documents);
        auto transaction = memoryOnly.begin(
            validId<TransactionId>("transaction.snapshot-ahead"), advancedDocumentId);
        REQUIRE(transaction.hasValue());
        REQUIRE(transaction.value()
                    ->createObject(ObjectRecord {
                        validId<ObjectId>("object.snapshot-ahead"),
                        validId<ObjectTypeId>("kernel.persistence.snapshot"),
                        Value {"not-journaled"}})
                    .hasValue());
        REQUIRE(transaction.value()->commit().hasValue());
        auto advanced = documents.snapshot(advancedDocumentId);
        REQUIRE(advanced.hasValue());
        auto rejected = persistence.captureSnapshot(
            validId<SnapshotId>("snapshot.ahead"), advanced.value());
        REQUIRE_FALSE(rejected.hasValue());
        CHECK(std::string(rejected.error().code.value())
              == "Persistence.SnapshotRevisionNotJournaled");
        CHECK_FALSE(std::filesystem::exists(
            snapshotDirectory / "snapshot.ahead.snapshot"));
    }
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
}

TEST_CASE("PersistenceService restores a snapshot and replays only its journal tail", "[persistence][recovery]")
{
    const auto path = uniqueDatabasePath();
    const auto snapshotDirectory = uniqueSnapshotDirectory();
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
    const auto projectId = validId<ProjectId>("project.recovery");
    const auto documentId = validId<DocumentId>("document.recovery");
    const auto objectId = validId<ObjectId>("object.recovery");

    {
        PersistenceService persistence;
        configureService(persistence, path, snapshotDirectory);
        DocumentStore documents;
        REQUIRE(documents.addDocument(projectId, documentId).hasValue());
        TransactionManager transactions(documents, &persistence);

        auto first = transactions.begin(
            validId<TransactionId>("transaction.recovery.1"), documentId);
        REQUIRE(first.hasValue());
        REQUIRE(first.value()
                    ->createObject(ObjectRecord {
                        objectId,
                        validId<ObjectTypeId>("kernel.persistence.recovery"),
                        Value {"snapshot-state"}})
                    .hasValue());
        REQUIRE(first.value()->collectEvent(lasercnc::messaging::PendingDomainEvent {
            validId<EventName>("kernel.recovery.history"),
            Version {1U, 0U, 0U},
            objectId,
            Value {"must-not-republish"}}).hasValue());
        REQUIRE(first.value()->commit().hasValue());
        auto snapshotState = documents.snapshot(documentId);
        REQUIRE(snapshotState.hasValue());
        REQUIRE(persistence
                    .captureSnapshot(
                        validId<SnapshotId>("snapshot.recovery"),
                        snapshotState.value())
                    .hasValue());

        auto second = transactions.begin(
            validId<TransactionId>("transaction.recovery.2"), documentId);
        REQUIRE(second.hasValue());
        REQUIRE(second.value()->replaceObjectData(objectId, Value {"journal-tail"}).hasValue());
        REQUIRE(second.value()->touchRevision(RevisionScope::Cam).hasValue());
        REQUIRE(second.value()->commit().hasValue());
    }

    {
        PersistenceService verifier;
        configureService(verifier, path, snapshotDirectory);
        auto recovered = verifier.recover();
        REQUIRE(recovered.hasValue());
        REQUIRE(recovered.value().documents.size() == 1U);
        CHECK(recovered.value().latestJournalSequence == 2U);
        CHECK(recovered.value().journalRecordsReplayed == 1U);
        REQUIRE(recovered.value().documents.front().objects.size() == 1U);
        CHECK(recovered.value().documents.front().objects.front().data
              == Value {"journal-tail"});
    }

    {
        lasercnc::kernel::AppKernel kernel;
        auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(backend.hasValue());
        auto snapshots = FilesystemSnapshotStore::create(
            FilesystemSnapshotStoreOptions {snapshotDirectory, 1024U * 1024U});
        REQUIRE(snapshots.hasValue());
        REQUIRE(kernel.persistence()
                    .configure(
                        std::move(backend).value(),
                        std::make_shared<JsonconsAdapter>(),
                        std::make_shared<Sha256HashService>(),
                        std::move(snapshots).value())
                    .hasValue());
        std::size_t delivered = 0U;
        REQUIRE(lasercnc::test::registerObjectType(kernel,
            lasercnc::test::valueObjectType("kernel.persistence.recovery")).hasValue());
        auto subscription = kernel.events().subscribe(
            validId<SubscriptionId>("subscription.recovery"),
            lasercnc::messaging::EventFilter {
                lasercnc::messaging::EventKind::Domain,
                validId<EventName>("kernel.recovery.history")},
            lasercnc::messaging::DeliveryMode::Immediate,
            [&delivered](const lasercnc::messaging::EventEnvelope&) { ++delivered; });
        REQUIRE(subscription.hasValue());
        REQUIRE(kernel.bootstrap().hasValue());
        CHECK(kernel.effectGuards().frozen());
        CHECK(kernel.resources().frozen());
        auto lateGuard = kernel.effectGuards().registerGuard(
            validId<EffectGuardId>("guard.effect.late"),
            std::make_shared<TestEffectGuard>());
        REQUIRE_FALSE(lateGuard.hasValue());
        CHECK(std::string(lateGuard.error().code.value())
              == "EffectGuard.RegistryFrozen");
        CHECK(delivered == 0U);
        auto restored = kernel.documents().snapshot(documentId);
        REQUIRE(restored.hasValue());
        const auto* object = restored.value().objects().find(objectId);
        REQUIRE(object != nullptr);
        CHECK(object->data == Value {"journal-tail"});
        CHECK(restored.value().revisions().at(RevisionScope::Document) == Revision {2U});
        CHECK(restored.value().revisions().at(RevisionScope::Cam) == Revision {1U});
        REQUIRE(kernel.shutdown().hasValue());
    }
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
}

TEST_CASE("Recovery keeps staggered document snapshots on one project revision chain", "[persistence][snapshot][recovery]")
{
    const auto path = uniqueDatabasePath();
    const auto snapshotDirectory = uniqueSnapshotDirectory();
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
    const auto projectId = validId<ProjectId>("project.recovery-multi");
    const auto firstDocumentId = validId<DocumentId>("document.recovery-a");
    const auto secondDocumentId = validId<DocumentId>("document.recovery-b");
    const auto firstObjectId = validId<ObjectId>("object.recovery-a");
    const auto secondObjectId = validId<ObjectId>("object.recovery-b");

    {
        PersistenceService persistence;
        configureService(persistence, path, snapshotDirectory);
        DocumentStore documents;
        REQUIRE(documents.addDocument(projectId, firstDocumentId).hasValue());
        REQUIRE(documents.addDocument(projectId, secondDocumentId).hasValue());
        TransactionManager transactions(documents, &persistence);
        const auto create = [&](const char* transactionId,
                                const DocumentId& documentId,
                                const ObjectId& objectId,
                                const char* value) {
            auto transaction = transactions.begin(
                validId<TransactionId>(transactionId), documentId);
            REQUIRE(transaction.hasValue());
            REQUIRE(transaction.value()
                        ->createObject(ObjectRecord {
                            objectId,
                            validId<ObjectTypeId>("kernel.persistence.recovery"),
                            Value {value}})
                        .hasValue());
            REQUIRE(transaction.value()->commit().hasValue());
        };
        const auto update = [&](const char* transactionId,
                                const DocumentId& documentId,
                                const ObjectId& objectId,
                                const char* value) {
            auto transaction = transactions.begin(
                validId<TransactionId>(transactionId), documentId);
            REQUIRE(transaction.hasValue());
            REQUIRE(transaction.value()->replaceObjectData(objectId, Value {value}).hasValue());
            REQUIRE(transaction.value()->commit().hasValue());
        };

        create("transaction.recovery-multi.1", firstDocumentId, firstObjectId, "a1");
        create("transaction.recovery-multi.2", secondDocumentId, secondObjectId, "b1");
        auto firstDocument = documents.snapshot(firstDocumentId);
        REQUIRE(firstDocument.hasValue());
        auto firstSnapshot = persistence.captureSnapshot(
            validId<SnapshotId>("snapshot.recovery-a"), firstDocument.value());
        REQUIRE(firstSnapshot.hasValue());
        CHECK(firstSnapshot.value().journalSequence == 2U);

        update("transaction.recovery-multi.3", firstDocumentId, firstObjectId, "a2");
        auto secondDocument = documents.snapshot(secondDocumentId);
        REQUIRE(secondDocument.hasValue());
        auto secondSnapshot = persistence.captureSnapshot(
            validId<SnapshotId>("snapshot.recovery-b"), secondDocument.value());
        REQUIRE(secondSnapshot.hasValue());
        CHECK(secondSnapshot.value().journalSequence == 3U);
        update("transaction.recovery-multi.4", secondDocumentId, secondObjectId, "b2");
    }

    {
        PersistenceService reopened;
        configureService(reopened, path, snapshotDirectory);
        auto recovered = reopened.recover();
        REQUIRE(recovered.hasValue());
        REQUIRE(recovered.value().documents.size() == 2U);
        CHECK(recovered.value().latestJournalSequence == 4U);
        CHECK(recovered.value().journalRecordsReplayed == 2U);
        for(const auto& document : recovered.value().documents) {
            CHECK(document.revisions.at(RevisionScope::Project) == Revision {4U});
            REQUIRE(document.objects.size() == 1U);
            if(document.documentId == firstDocumentId) {
                CHECK(document.objects.front().data == Value {"a2"});
            } else {
                CHECK(document.documentId == secondDocumentId);
                CHECK(document.objects.front().data == Value {"b2"});
            }
        }
    }
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
}

TEST_CASE("Crash recovery fails closed on journal gaps", "[persistence][recovery]")
{
    const auto path = uniqueDatabasePath();
    const auto snapshotDirectory = uniqueSnapshotDirectory();
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
    const RevisionSet zero;
    const RevisionSet one {
        Revision {1U}, Revision {1U}, Revision {1U}, Revision {}, Revision {}, Revision {}};
    const RevisionSet two {
        Revision {2U}, Revision {2U}, Revision {2U}, Revision {}, Revision {}, Revision {}};

    {
        PersistenceService persistence;
        configureService(persistence, path, snapshotDirectory);
        REQUIRE(persistence.append(
            commit("transaction.recovery-gap.1", zero, one, "one")).hasValue());
        REQUIRE(persistence.append(
            commit("transaction.recovery-gap.2", one, two, "two")).hasValue());
    }
    {
        auto tamper = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(tamper.hasValue());
        const std::array parameters {Value {"transaction.recovery-gap.1"}};
        REQUIRE(tamper.value()
                    ->execute(
                        "DELETE FROM state_journal WHERE transaction_id=?",
                        parameters)
                    .hasValue());
    }
    {
        PersistenceService reopened;
        configureService(reopened, path, snapshotDirectory);
        auto recovered = reopened.recover();
        REQUIRE_FALSE(recovered.hasValue());
        CHECK(std::string(recovered.error().code.value())
              == "Persistence.JournalSequenceGap");
    }
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
}

TEST_CASE("Crash recovery validates object before state", "[persistence][recovery]")
{
    const auto path = uniqueDatabasePath();
    const auto snapshotDirectory = uniqueSnapshotDirectory();
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
    const RevisionSet zero;
    const RevisionSet one {
        Revision {1U}, Revision {1U}, Revision {1U}, Revision {}, Revision {}, Revision {}};
    const RevisionSet two {
        Revision {2U}, Revision {2U}, Revision {2U}, Revision {}, Revision {}, Revision {}};
    {
        PersistenceService persistence;
        configureService(persistence, path, snapshotDirectory);
        REQUIRE(persistence.append(
            commit("transaction.replay-conflict.1", zero, one, "one")).hasValue());
        REQUIRE(persistence.append(
            commit("transaction.replay-conflict.2", one, two, "two")).hasValue());
        auto recovered = persistence.recover();
        REQUIRE_FALSE(recovered.hasValue());
        CHECK(std::string(recovered.error().code.value())
              == "Persistence.ReplayObjectConflict");
    }
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
}

TEST_CASE("CommandRuntime replays durable idempotency after process restart", "[persistence][idempotency][command]")
{
    const auto path = uniqueDatabasePath();
    const auto snapshotDirectory = uniqueSnapshotDirectory();
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
    const auto projectId = validId<ProjectId>("project.command-replay");
    const auto documentId = validId<DocumentId>("document.command-replay");
    const auto sessionId = validId<SessionId>("session.command-replay");
    const auto key = validId<IdempotencyKey>("idempotency.command-replay");
    TransactionId committedTransaction = validId<TransactionId>("placeholder");

    {
        lasercnc::kernel::AppKernel kernel;
        REQUIRE(kernel.addDocument(projectId, documentId).hasValue());
        auto handler = std::make_shared<PersistentCreateHandler>();
        configureRuntimeKernel(
            kernel, path, snapshotDirectory, sessionId, handler);
        REQUIRE(kernel.bootstrap().hasValue());
        auto first = kernel.execution().executeCommand(persistentCommandRequest(
            "request.command-replay.first",
            projectId,
            documentId,
            sessionId,
            key,
            "object.command-replay"));
        REQUIRE(first.hasValue());
        CHECK_FALSE(first.value().replayed);
        REQUIRE(first.value().commit.has_value());
        committedTransaction = first.value().commit->transactionId;
        CHECK(handler->calls == 1U);
        REQUIRE(kernel.shutdown().hasValue());
    }

    {
        lasercnc::kernel::AppKernel kernel;
        auto handler = std::make_shared<PersistentCreateHandler>();
        configureRuntimeKernel(
            kernel, path, snapshotDirectory, sessionId, handler);
        std::size_t events = 0U;
        auto subscription = kernel.events().subscribe(
            validId<SubscriptionId>("subscription.command-replay"),
            lasercnc::messaging::EventFilter {
                lasercnc::messaging::EventKind::Domain, std::nullopt},
            lasercnc::messaging::DeliveryMode::Immediate,
            [&events](const lasercnc::messaging::EventEnvelope&) { ++events; });
        REQUIRE(subscription.hasValue());
        REQUIRE(kernel.bootstrap().hasValue());
        auto replay = kernel.execution().executeCommand(persistentCommandRequest(
            "request.command-replay.retry",
            projectId,
            documentId,
            sessionId,
            key,
            "object.command-replay"));
        REQUIRE(replay.hasValue());
        CHECK(replay.value().replayed);
        REQUIRE(replay.value().commit.has_value());
        CHECK(replay.value().commit->transactionId == committedTransaction);
        REQUIRE(replay.value().commit->events.size() == 1U);
        CHECK(replay.value().commit->events.front().name()
              == validId<EventName>("kernel.persistence.command-created"));
        CHECK(handler->calls == 0U);
        CHECK(events == 0U);

        auto conflict = kernel.execution().executeCommand(persistentCommandRequest(
            "request.command-replay.conflict",
            projectId,
            documentId,
            sessionId,
            key,
            "object.command-rebound"));
        REQUIRE_FALSE(conflict.hasValue());
        CHECK(std::string(conflict.error().code.value())
              == "Command.IdempotencyKeyConflict");
        CHECK(handler->calls == 0U);
        REQUIRE(kernel.shutdown().hasValue());
    }
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
}

TEST_CASE("Asynchronous command acceptance and task outcome survive restart", "[persistence][idempotency][task]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    const auto projectId = validId<ProjectId>("project.async-replay");
    const auto documentId = validId<DocumentId>("document.async-replay");
    const auto sessionId = validId<SessionId>("session.async-replay");
    const auto key = validId<IdempotencyKey>("idempotency.async-replay");
    TaskId acceptedTaskId = validId<TaskId>("task.placeholder");

    {
        lasercnc::kernel::AppKernel kernel;
        REQUIRE(kernel.addDocument(projectId, documentId).hasValue());
        auto commandHandler = std::make_shared<PersistentAsyncHandler>();
        auto taskHandler = std::make_shared<PersistentTaskHandler>();
        configureAsyncRuntimeKernel(
            kernel, path, sessionId, commandHandler, taskHandler);
        REQUIRE(kernel.bootstrap().hasValue());
        auto accepted = kernel.execution().executeCommand(persistentAsyncCommandRequest(
            "request.async-replay.first",
            projectId,
            documentId,
            sessionId,
            key));
        REQUIRE(accepted.hasValue());
        CHECK_FALSE(accepted.value().replayed);
        REQUIRE(accepted.value().taskId.has_value());
        acceptedTaskId = *accepted.value().taskId;
        auto terminal = kernel.execution().waitTask(acceptedTaskId, std::chrono::seconds(2));
        REQUIRE(terminal.hasValue());
        CHECK(terminal.value().state == TaskState::Succeeded);
        REQUIRE(terminal.value().result.has_value());
        CHECK(*terminal.value().result == Value {Value::Object {
            {"value", Value {"completed"}}}});
        CHECK(commandHandler->calls.load() == 1U);
        CHECK(taskHandler->calls.load() == 1U);
        REQUIRE(kernel.shutdown().hasValue());
    }

    {
        lasercnc::kernel::AppKernel kernel;
        auto commandHandler = std::make_shared<PersistentAsyncHandler>();
        auto taskHandler = std::make_shared<PersistentTaskHandler>();
        configureAsyncRuntimeKernel(
            kernel, path, sessionId, commandHandler, taskHandler);
        REQUIRE(kernel.bootstrap().hasValue());
        auto replay = kernel.execution().executeCommand(persistentAsyncCommandRequest(
            "request.async-replay.retry",
            projectId,
            documentId,
            sessionId,
            key));
        REQUIRE(replay.hasValue());
        CHECK(replay.value().replayed);
        CHECK(replay.value().taskId == acceptedTaskId);
        CHECK(commandHandler->calls.load() == 0U);
        CHECK(taskHandler->calls.load() == 0U);

        auto terminal = kernel.execution().task(acceptedTaskId);
        REQUIRE(terminal.hasValue());
        CHECK(terminal.value().state == TaskState::Succeeded);
        REQUIRE(terminal.value().result.has_value());
        CHECK(*terminal.value().result == Value {Value::Object {
            {"value", Value {"completed"}}}});
        REQUIRE(kernel.shutdown().hasValue());
    }
    removeDatabase(path);
}

TEST_CASE("Task acceptance backend failures discard prepared work and preserve retry contracts", "[persistence][task][fault-matrix]")
{
    const auto retryBeforeRestart = GENERATE(false, true);
    using lasercnc::test::BackendPoint;
    struct Stage { const char* name; BackendPoint point; const char* sql; unsigned int occurrence; };
    const std::array stages{
        Stage{"accept begin", BackendPoint::Begin, "", 2U},
        Stage{"task insert", BackendPoint::Execute, "INSERT INTO task_history", 1U},
        Stage{"acceptance outcome", BackendPoint::Execute, "UPDATE command_idempotency SET status='completed'", 1U},
        Stage{"accept commit", BackendPoint::Commit, "", 2U},
    };
    for(const auto& stage : stages) {
        for(const bool throws : {false, true}) {
            DYNAMIC_SECTION(stage.name << " throws=" << throws << " retryBeforeRestart=" << retryBeforeRestart) {
                const auto path = uniqueDatabasePath();
                const auto project = validId<ProjectId>("project.accept-fault");
                const auto document = validId<DocumentId>("document.accept-fault");
                const auto session = validId<SessionId>("session.accept-fault");
                const auto key = validId<IdempotencyKey>("key.accept-fault");
                auto command = persistentAsyncCommandRequest("request.accept-fault", project, document, session, key);
                const auto task = validId<TaskId>("task.request.accept-fault");
                {
                    AppKernel kernel;
                    REQUIRE(kernel.addDocument(project, document).hasValue());
                    auto commandHandler = std::make_shared<PersistentAsyncHandler>();
                    auto taskHandler = std::make_shared<PersistentTaskHandler>();
                    auto sqlite = SqlitePersistenceBackend::open({path});
                    REQUIRE(sqlite.hasValue());
                    auto backend = std::make_unique<lasercnc::test::FaultInjectingBackend>(std::move(sqlite).value());
                    auto* control = backend.get();
                    configureAsyncRuntimeKernel(kernel, path, session, commandHandler, taskHandler, std::move(backend));
                    REQUIRE(kernel.bootstrap().hasValue());
                    control->arm(stage.point, stage.sql, stage.occurrence, throws);
                    auto rejected = kernel.execution().executeCommand(command);
                    REQUIRE_FALSE(rejected.hasValue());
                    CHECK(control->hits == 1U);
                    CHECK(commandHandler->calls.load() == 1U);
                    CHECK(taskHandler->calls.load() == 0U);
                    CHECK_FALSE(kernel.execution().task(task).hasValue());
                    CHECK_FALSE(kernel.persistence().taskHistory(task).value().has_value());
                    CHECK(kernel.documents().snapshot(document).value().revisions() == RevisionSet{});
                    CHECK(kernel.persistence().journalAfter(document, 0U).value().empty());
                    auto claims = control->query("SELECT * FROM command_idempotency");
                    REQUIRE(claims.hasValue());
                    CHECK(claims.value().empty());
                    // In-process idempotency retains errors; a new key permits a fresh attempt.
                    // 中文翻译：进程内幂等缓存保留错误，使用新 Key 发起新的尝试。
                    auto cached = kernel.execution().executeCommand(command);
                    REQUIRE_FALSE(cached.hasValue());
                    CHECK(cached.error().code == rejected.error().code);
                    CHECK(commandHandler->calls.load() == 1U);
                    if(retryBeforeRestart) {
                        command.idempotencyKey = validId<IdempotencyKey>("key.accept-fault.retry");
                        // The TaskId stays unchanged, proving prepared scheduler state was removed.
                        // 中文翻译：保持 TaskId 不变，证明调度器准备态已移除。
                        auto accepted = kernel.execution().executeCommand(command);
                        INFO((accepted ? "accepted" : accepted.error().message));
                        REQUIRE(accepted.hasValue());
                        CHECK_FALSE(accepted.value().replayed);
                        CHECK(accepted.value().taskId == task);
                        auto terminal = kernel.execution().waitTask(task, std::chrono::seconds(2));
                        REQUIRE(terminal.hasValue());
                        CHECK(terminal.value().state == TaskState::Succeeded);
                        CHECK(taskHandler->calls.load() == 1U);
                    }
                    REQUIRE(kernel.shutdown().hasValue());
                }
                {
                    AppKernel kernel;
                    auto commandHandler = std::make_shared<PersistentAsyncHandler>();
                    auto taskHandler = std::make_shared<PersistentTaskHandler>();
                    configureAsyncRuntimeKernel(kernel, path, session, commandHandler, taskHandler);
                    REQUIRE(kernel.bootstrap().hasValue());
                    auto replayed = kernel.execution().executeCommand(command);
                    REQUIRE(replayed.hasValue());
                    CHECK(replayed.value().replayed == retryBeforeRestart);
                    CHECK(replayed.value().taskId == task);
                    CHECK(commandHandler->calls.load() == (retryBeforeRestart ? 0U : 1U));
                    auto terminal = kernel.execution().waitTask(task, std::chrono::seconds(2));
                    REQUIRE(terminal.hasValue());
                    CHECK(terminal.value().state == TaskState::Succeeded);
                    CHECK(taskHandler->calls.load() == (retryBeforeRestart ? 0U : 1U));
                    REQUIRE(kernel.shutdown().hasValue());
                }
                removeDatabase(path);
            }
        }
    }
}

TEST_CASE("Executor admission faults finish accepted tasks and do not strand scheduler capacity", "[task][executor][fault-matrix]")
{
    class FaultExecutor final : public lasercnc::platform::ITaskExecutor {
    public:
        Result<void> submit(lasercnc::platform::ExecutorWork work, lasercnc::platform::ExecutorCompletion done) override
        {
            ++calls;
            if(fail) {
                fail = false;
                if(throws) { throw std::runtime_error("Injected executor admission exception"); }
                return Result<void>::failure(makeError("Test.ExecutorRefused", ErrorCategory::Infrastructure, "Injected refusal"));
            }
            done(work());
            return Result<void>::success();
        }
        Result<void> waitIdle() override { return Result<void>::success(); }
        Result<void> shutdown() override { return Result<void>::success(); }
        std::size_t concurrency() const noexcept override { return 1U; }
        bool fail{true};
        bool throws{false};
        unsigned int calls{0U};
    };
    for(const bool throws : {false, true}) {
        DYNAMIC_SECTION("executor throws=" << throws) {
            const auto path = uniqueDatabasePath();
            const auto project = validId<ProjectId>("project.executor-fault");
            const auto document = validId<DocumentId>("document.executor-fault");
            const auto session = validId<SessionId>("session.executor-fault");
            const auto command = persistentAsyncCommandRequest("request.executor-fault", project, document,
                session, validId<IdempotencyKey>("key.executor-fault"));
            const auto task = validId<TaskId>("task.request.executor-fault");
            {
                AppKernel kernel;
                REQUIRE(kernel.addDocument(project, document).hasValue());
                auto executor = std::make_unique<FaultExecutor>();
                auto* observed = executor.get();
                observed->throws = throws;
                auto commandHandler = std::make_shared<PersistentAsyncHandler>();
                commandHandler->resources = {ResourceClaim{ResourceKind::DiskIO,
                    validId<ResourceId>("resource.executor-fault"), ResourceAccess::Exclusive, 1U}};
                auto taskHandler = std::make_shared<PersistentTaskHandler>();
                configureAsyncRuntimeKernel(kernel, path, session, commandHandler, taskHandler, nullptr, std::move(executor));
                REQUIRE(kernel.bootstrap().hasValue());
                auto accepted = kernel.execution().executeCommand(command);
                REQUIRE(accepted.hasValue());
                CHECK(accepted.value().taskId == task);
                auto terminal = kernel.execution().waitTask(task, std::chrono::seconds(1));
                REQUIRE(terminal.hasValue());
                CHECK(terminal.value().state == TaskState::Failed);
                REQUIRE(terminal.value().error.has_value());
                CHECK(std::string(terminal.value().error->code.value()) == (throws ? "Task.ExecutorSubmitFailed" : "Test.ExecutorRefused"));
                CHECK(taskHandler->calls.load() == 0U);
                CHECK(observed->calls == 1U);
                auto next = persistentAsyncCommandRequest("request.executor-next", project, document,
                    session, validId<IdempotencyKey>("key.executor-next"));
                auto nextAccepted = kernel.execution().executeCommand(next);
                REQUIRE(nextAccepted.hasValue());
                auto nextTerminal = kernel.execution().waitTask(*nextAccepted.value().taskId, std::chrono::seconds(1));
                REQUIRE(nextTerminal.hasValue());
                CHECK(nextTerminal.value().state == TaskState::Succeeded);
                CHECK(taskHandler->calls.load() == 1U);
                CHECK(kernel.execution().executeCommand(command).value().replayed);
                CHECK(observed->calls == 2U);
                REQUIRE(kernel.shutdown(std::chrono::seconds(1)).hasValue());
            }
            {
                AppKernel kernel;
                auto commandHandler = std::make_shared<PersistentAsyncHandler>();
                auto taskHandler = std::make_shared<PersistentTaskHandler>();
                configureAsyncRuntimeKernel(kernel, path, session, commandHandler, taskHandler);
                REQUIRE(kernel.bootstrap().hasValue());
                CHECK(kernel.execution().executeCommand(command).value().replayed);
                CHECK(kernel.execution().task(task).value().state == TaskState::Failed);
                CHECK(commandHandler->calls.load() == 0U);
                CHECK(taskHandler->calls.load() == 0U);
                REQUIRE(kernel.shutdown().hasValue());
            }
            removeDatabase(path);
        }
    }
}

TEST_CASE("Task completion exposes persistence failure without changing task outcome", "[persistence][task][failure]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    lasercnc::kernel::AppKernel kernel;
    const auto session = validId<SessionId>("session.persistence-failure");
    const auto submitCapability = validId<CapabilityId>("task.persistence.submit");
    const std::array submitGrants {submitCapability};
    REQUIRE(kernel.capabilities().replace(session, submitGrants).hasValue());
    auto adapter = std::make_shared<JsonconsAdapter>();
    REQUIRE(kernel.executionServices()
                .configure(adapter, std::make_shared<NullLogService>())
                .hasValue());
    auto taskHandler = std::make_shared<PersistentTaskHandler>();
    REQUIRE(lasercnc::test::registerTask(kernel, persistentTaskDescriptor(), taskHandler)
                .hasValue());
    const auto request = TaskRequest {
        validId<TaskId>("task.persistence-failure"),
        validId<TaskName>("kernel.persistence.async-task"),
        Value {Value::Object {{"input", Value {"durable"}}}},
        validId<TraceId>("trace.persistence-failure")};
    REQUIRE(lasercnc::test::registerAsyncCommand(
                kernel,
                lasercnc::test::taskSubmissionDescriptor(
                    "command.persistence.task-submit", "task.persistence.submit"),
                std::make_shared<lasercnc::test::FixedTaskCommandHandler>(request))
                .hasValue());
    auto sqlite = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
    REQUIRE(sqlite.hasValue());
    auto failing = std::make_unique<FailingTaskTerminalBackend>(
        std::move(sqlite).value());
    auto* observed = failing.get();
    REQUIRE(kernel.persistence()
                .configure(
                    std::move(failing),
                    adapter,
                    std::make_shared<Sha256HashService>())
                .hasValue());
    auto executor = BsThreadPoolExecutor::create(BsThreadPoolExecutorOptions {1U});
    REQUIRE(executor.hasValue());
    REQUIRE(kernel.configureTaskExecutor(std::move(executor).value()).hasValue());
    REQUIRE(kernel.bootstrap().hasValue());
    observed->failTerminal.store(true, std::memory_order_release);

    auto accepted = kernel.execution().executeCommand(
        lasercnc::test::taskSubmissionRequest(
            "request.persistence.task-submit",
            "command.persistence.task-submit",
            session,
            "trace.persistence-failure"));
    REQUIRE(accepted.hasValue());
    CHECK(accepted.value().taskId == request.taskId);
    auto terminal = kernel.execution().waitTask(request.taskId, std::chrono::seconds(2));
    REQUIRE(terminal.hasValue());
    CHECK(terminal.value().state == TaskState::Succeeded);
    REQUIRE(kernel.scheduler().persistenceFailures().size() == 1U);
    CHECK(std::string(kernel.scheduler().persistenceFailures().front().code.value())
          == "Test.TaskTerminalPersistFailed");
    REQUIRE(kernel.shutdown().hasValue());
    removeDatabase(path);
}

TEST_CASE("Task history marks interrupted work and rejects tampering", "[persistence][task][recovery]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    const auto taskId = validId<TaskId>("task.interrupted");
    const auto completedTaskId = validId<TaskId>("task.completed-history");
    const auto request = TaskRequest {
        taskId,
        validId<TaskName>("kernel.persistence.interrupted"),
        Value {Value::Object {{"input", Value {"durable"}}}},
        validId<TraceId>("trace.persistence.interrupted")};

    {
        PersistenceService service;
        configureService(service, path);
        REQUIRE(service.acceptTask(request, std::nullopt).hasValue());
        auto completedRequest = request;
        completedRequest.taskId = completedTaskId;
        REQUIRE(service.acceptTask(completedRequest, std::nullopt).hasValue());
        const auto completed = TaskSnapshot {
            completedTaskId,
            request.task,
            TaskState::Succeeded,
            1.0,
            "done",
            request.traceId,
            std::nullopt,
            Value {"result"},
            std::nullopt};
        REQUIRE(service.recordTaskTerminal(completed).hasValue());
        REQUIRE(service.recordTaskTerminal(completed).hasValue());
        auto conflicting = completed;
        conflicting.result = Value {"different"};
        auto rejected = service.recordTaskTerminal(conflicting);
        REQUIRE_FALSE(rejected.hasValue());
        CHECK(std::string(rejected.error().code.value())
              == "Persistence.TaskOutcomeConflict");
        auto accepted = service.taskHistory(taskId);
        REQUIRE(accepted.hasValue());
        REQUIRE(accepted.value().has_value());
        CHECK(accepted.value()->state == TaskState::Pending);
    }
    {
        PersistenceService reopened;
        configureService(reopened, path);
        auto interrupted = reopened.taskHistory(taskId);
        REQUIRE(interrupted.hasValue());
        REQUIRE(interrupted.value().has_value());
        CHECK(interrupted.value()->state == TaskState::Failed);
        REQUIRE(interrupted.value()->error.has_value());
        CHECK(std::string(interrupted.value()->error->code.value())
              == "Task.InterruptedByRestart");

        auto completed = reopened.taskHistory(completedTaskId);
        REQUIRE(completed.hasValue());
        REQUIRE(completed.value().has_value());
        CHECK(completed.value()->state == TaskState::Succeeded);
        REQUIRE(completed.value()->result.has_value());
        CHECK(*completed.value()->result == Value {"result"});

        auto tamper = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(tamper.hasValue());
        const std::array parameters {Value {"tampered"}, Value {std::string(taskId.value())}};
        REQUIRE(tamper.value()
                    ->execute(
                        "UPDATE task_history SET request_payload=? WHERE task_id=?",
                        parameters)
                    .hasValue());
        auto corrupted = reopened.taskHistory(taskId);
        REQUIRE_FALSE(corrupted.hasValue());
        CHECK(std::string(corrupted.error().code.value())
              == "Persistence.TaskRequestDigestMismatch");

        const std::array terminalParameters {
            Value {"tampered"}, Value {std::string(completedTaskId.value())}};
        REQUIRE(tamper.value()
                    ->execute(
                        "UPDATE task_history SET terminal_payload=? WHERE task_id=?",
                        terminalParameters)
                    .hasValue());
        auto corruptedTerminal = reopened.taskHistory(completedTaskId);
        REQUIRE_FALSE(corruptedTerminal.hasValue());
        CHECK(std::string(corruptedTerminal.error().code.value())
              == "Persistence.TaskTerminalDigestMismatch");
    }
    removeDatabase(path);
}

TEST_CASE("AppKernel persists diagnostic history without changing check results", "[persistence][diagnostics]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    const auto diagnosticId = validId<DiagnosticId>("kernel.persistence.health");

    {
        lasercnc::kernel::AppKernel kernel;
        auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(backend.hasValue());
        REQUIRE(kernel.persistence()
                    .configure(
                        std::move(backend).value(),
                        std::make_shared<JsonconsAdapter>(),
                        std::make_shared<Sha256HashService>())
                    .hasValue());
        auto check = std::make_shared<FixedDiagnosticCheck>(diagnosticId);
        REQUIRE(kernel.diagnostics().registerCheck(diagnosticId, check).hasValue());
        REQUIRE(kernel.bootstrap().hasValue());
        auto first = kernel.diagnostics().run(diagnosticId);
        auto second = kernel.diagnostics().run(diagnosticId);
        REQUIRE(first.hasValue());
        REQUIRE(second.hasValue());
        CHECK(first.value().status
              == lasercnc::observability::DiagnosticStatus::Degraded);
        CHECK(second.value().status
              == lasercnc::observability::DiagnosticStatus::Degraded);
        CHECK(kernel.diagnostics().exporterFailures().empty());
        CHECK(check->calls == 2U);
        REQUIRE(kernel.shutdown().hasValue());
    }

    {
        PersistenceService reopened;
        configureService(reopened, path);
        auto history = reopened.diagnosticHistory(diagnosticId);
        REQUIRE(history.hasValue());
        REQUIRE(history.value().size() == 2U);
        CHECK(history.value().front().summary == "maintenance window");
        CHECK(history.value().back().details == Value {Value::Object {
            {"attempt", Value {"2"}}}});
        auto latest = reopened.latestDiagnostics();
        REQUIRE(latest.hasValue());
        REQUIRE(latest.value().size() == 1U);
        CHECK(latest.value().front().id == diagnosticId);
        CHECK(latest.value().front().details == history.value().back().details);

        auto tamper = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(tamper.hasValue());
        const std::array parameters {
            Value {"tampered"}, Value {std::string(diagnosticId.value())}};
        REQUIRE(tamper.value()
                    ->execute(
                        "UPDATE diagnostic_history SET payload=? "
                        "WHERE diagnostic_id=? AND sequence=1",
                        parameters)
                    .hasValue());
        auto corrupted = reopened.diagnosticHistory(diagnosticId);
        REQUIRE_FALSE(corrupted.hasValue());
        CHECK(std::string(corrupted.error().code.value())
              == "Persistence.DiagnosticDigestMismatch");
        CHECK_FALSE(reopened.latestDiagnostics().hasValue());
    }
    removeDatabase(path);
}

TEST_CASE("Diagnostic persistence failure is isolated after the in-memory report", "[persistence][diagnostics][failure]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    lasercnc::kernel::AppKernel kernel;
    auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
    REQUIRE(backend.hasValue());
    REQUIRE(kernel.persistence()
                .configure(
                    std::move(backend).value(),
                    std::make_shared<JsonconsAdapter>(),
                    std::make_shared<Sha256HashService>())
                .hasValue());
    const auto diagnosticId = validId<DiagnosticId>("kernel.persistence.failure");
    REQUIRE(kernel.diagnostics()
                .registerCheck(
                    diagnosticId,
                    std::make_shared<FixedDiagnosticCheck>(diagnosticId))
                .hasValue());
    REQUIRE(kernel.bootstrap().hasValue());
    auto tamper = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
    REQUIRE(tamper.hasValue());
    REQUIRE(tamper.value()->execute("DROP TABLE diagnostic_history").hasValue());

    auto report = kernel.diagnostics().run(diagnosticId);
    REQUIRE(report.hasValue());
    CHECK(report.value().status
          == lasercnc::observability::DiagnosticStatus::Degraded);
    REQUIRE(kernel.diagnostics().latest().size() == 1U);
    REQUIRE(kernel.diagnostics().exporterFailures().size() == 1U);
    CHECK(std::string(
              kernel.diagnostics().exporterFailures().front().code.value())
          == "Persistence.DatabaseFailed");
    REQUIRE(kernel.shutdown().hasValue());
    removeDatabase(path);
}

TEST_CASE("PersistenceService rolls back migration exceptions and rejects newer schemas", "[persistence][migration]")
{
    auto throwing = std::make_unique<ThrowingBackend>();
    auto* observed = throwing.get();
    PersistenceService exceptionService;
    REQUIRE(exceptionService
                .configure(
                    std::move(throwing),
                    std::make_shared<JsonconsAdapter>(),
                    std::make_shared<Sha256HashService>())
                .hasValue());
    auto failed = exceptionService.initialize();
    REQUIRE_FALSE(failed.hasValue());
    CHECK(std::string(failed.error().code.value()) == "Persistence.InitializeFailed");
    CHECK(observed->begins == 1U);
    CHECK(observed->rollbacks == 1U);
    CHECK_FALSE(observed->active);

    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    {
        auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(backend.hasValue());
        REQUIRE(backend.value()
                    ->execute(
                        "CREATE TABLE schema_migrations("
                        "version INTEGER PRIMARY KEY NOT NULL,applied_at TEXT NOT NULL)")
                    .hasValue());
        const std::array parameters {Value {std::int64_t {9}}, Value {"future"}};
        REQUIRE(backend.value()
                    ->execute(
                        "INSERT INTO schema_migrations(version,applied_at) VALUES(?,?)",
                        parameters)
                    .hasValue());

        PersistenceService newerSchema;
        REQUIRE(newerSchema
                    .configure(
                        std::move(backend).value(),
                        std::make_shared<JsonconsAdapter>(),
                        std::make_shared<Sha256HashService>())
                    .hasValue());
        auto rejected = newerSchema.initialize();
        REQUIRE_FALSE(rejected.hasValue());
        CHECK(std::string(rejected.error().code.value())
              == "Persistence.SchemaTooNew");
        CHECK_FALSE(newerSchema.ready());
    }
    removeDatabase(path);
}

TEST_CASE("Document lifecycle catalog survives close open remove and restart",
          "[persistence][document][lifecycle]")
{
    const auto path = uniqueDatabasePath();
    const auto snapshots = uniqueSnapshotDirectory();
    removeDatabase(path);
    removeSnapshotDirectory(snapshots);
    const auto project = validId<ProjectId>("project.lifecycle.persisted");
    const auto document = validId<DocumentId>("document.lifecycle.persisted");

    {
        lasercnc::kernel::AppKernel kernel;
        REQUIRE(kernel.addDocument(project, document).hasValue());
        configureKernelPersistence(kernel, path, snapshots);
        REQUIRE(kernel.bootstrap().hasValue());
        auto catalog = kernel.persistence().documentCatalog();
        REQUIRE(catalog.hasValue());
        REQUIRE(catalog.value().size() == 1U);
        CHECK(catalog.value().front().state == DocumentPersistenceState::Open);

        auto closed = kernel.documentRuntime().close(document);
        REQUIRE(closed.hasValue());
        CHECK(closed.value().state == DocumentLifecycleState::Detached);
        CHECK_FALSE(kernel.documents().contains(document));
        REQUIRE(kernel.shutdown().hasValue());
    }

    {
        lasercnc::kernel::AppKernel kernel;
        configureKernelPersistence(kernel, path, snapshots);
        REQUIRE(kernel.bootstrap().hasValue());
        auto lifecycle = kernel.documentRuntime().lifecycle(document);
        REQUIRE(lifecycle.hasValue());
        CHECK(lifecycle.value().state == DocumentLifecycleState::Detached);
        CHECK_FALSE(kernel.documents().contains(document));

        auto opened = kernel.documentRuntime().open(document);
        REQUIRE(opened.hasValue());
        CHECK(opened.value().state == DocumentLifecycleState::Open);
        CHECK(kernel.documents().contains(document));
        REQUIRE(kernel.documentRuntime().close(document).hasValue());
        REQUIRE(kernel.documentRuntime().remove(document).hasValue());
        CHECK(kernel.documentRuntime().list().empty());
        REQUIRE(kernel.shutdown().hasValue());
    }

    {
        lasercnc::kernel::AppKernel kernel;
        configureKernelPersistence(kernel, path, snapshots);
        REQUIRE(kernel.bootstrap().hasValue());
        CHECK(kernel.documentRuntime().list().empty());
        CHECK_FALSE(kernel.documents().contains(document));
        auto catalog = kernel.persistence().documentCatalog();
        REQUIRE(catalog.hasValue());
        REQUIRE(catalog.value().size() == 1U);
        CHECK(catalog.value().front().state == DocumentPersistenceState::Removed);
        REQUIRE(kernel.shutdown().hasValue());
    }

    removeDatabase(path);
    removeSnapshotDirectory(snapshots);
}

TEST_CASE("Document lifecycle catalog rejects ownership drift and tampering",
          "[persistence][document][integrity]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    const auto project = validId<ProjectId>("project.lifecycle.integrity");
    const auto otherProject = validId<ProjectId>("project.lifecycle.other");
    const auto document = validId<DocumentId>("document.lifecycle.integrity");

    {
        PersistenceService service;
        configureService(service, path);
        REQUIRE(service.saveDocumentLifecycle(
                    project, document, DocumentPersistenceState::Opening)
                    .hasValue());
        auto interrupted = service.documentCatalog();
        REQUIRE(interrupted.hasValue());
        REQUIRE(interrupted.value().size() == 1U);
        CHECK(interrupted.value().front().state == DocumentPersistenceState::Failed);
        CHECK(interrupted.value().front().interruptedTransition);

        auto ownership = service.saveDocumentLifecycle(
            otherProject, document, DocumentPersistenceState::Open);
        REQUIRE_FALSE(ownership.hasValue());
        CHECK(std::string(ownership.error().code.value())
              == "Persistence.DocumentOwnershipConflict");

        auto tamper = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(tamper.hasValue());
        const std::array parameters {
            Value {"detached"}, Value {std::string(document.value())}};
        REQUIRE(tamper.value()
                    ->execute(
                        "UPDATE document_catalog SET state=? WHERE document_id=?",
                        parameters)
                    .hasValue());

        auto corrupted = service.documentCatalog();
        REQUIRE_FALSE(corrupted.hasValue());
        CHECK(std::string(corrupted.error().code.value())
              == "Persistence.DocumentCatalogIntegrityFailed");
        auto overwrite = service.saveDocumentLifecycle(
            project, document, DocumentPersistenceState::Open);
        REQUIRE_FALSE(overwrite.hasValue());
        CHECK(std::string(overwrite.error().code.value())
              == "Persistence.DocumentCatalogIntegrityFailed");
    }
    removeDatabase(path);
}

TEST_CASE("Document close keeps state attached when snapshot persistence fails",
          "[persistence][document][failure]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    const auto project = validId<ProjectId>("project.lifecycle.failure");
    const auto document = validId<DocumentId>("document.lifecycle.failure");

    lasercnc::kernel::AppKernel kernel;
    REQUIRE(kernel.addDocument(project, document).hasValue());
    auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
    REQUIRE(backend.hasValue());
    REQUIRE(kernel.persistence()
                .configure(
                    std::move(backend).value(),
                    std::make_shared<JsonconsAdapter>(),
                    std::make_shared<Sha256HashService>())
                .hasValue());
    REQUIRE(kernel.bootstrap().hasValue());

    auto closed = kernel.documentRuntime().close(document);
    REQUIRE_FALSE(closed.hasValue());
    CHECK(std::string(closed.error().code.value())
          == "Persistence.SnapshotStoreNotConfigured");
    auto lifecycle = kernel.documentRuntime().lifecycle(document);
    REQUIRE(lifecycle.hasValue());
    CHECK(lifecycle.value().state == DocumentLifecycleState::Failed);
    CHECK(kernel.documents().contains(document));
    REQUIRE(kernel.shutdown().hasValue());
    removeDatabase(path);
}

TEST_CASE("DocumentRuntime blocks close while a document task remains active",
          "[task][document][lifecycle][concurrency]")
{
    const auto project = validId<ProjectId>("project.task-close");
    const auto document = validId<DocumentId>("document.task-close");
    const auto taskId = validId<TaskId>("task.document-close");
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::promise<void> release;

    lasercnc::kernel::AppKernel kernel;
    const auto session = validId<SessionId>("session.task-close");
    const auto submitCapability = validId<CapabilityId>("task.document-close.submit");
    const std::array submitGrants {submitCapability};
    REQUIRE(kernel.capabilities().replace(session, submitGrants).hasValue());
    REQUIRE(kernel.addDocument(project, document).hasValue());
    auto adapter = std::make_shared<JsonconsAdapter>();
    REQUIRE(kernel.executionServices()
                .configure(adapter, std::make_shared<NullLogService>())
                .hasValue());
    REQUIRE(lasercnc::test::registerTask(kernel,
                    persistentTaskDescriptor(),
                    std::make_shared<BlockingDocumentTaskHandler>(
                        entered, release.get_future().share()))
                .hasValue());
    auto taskRequest = TaskRequest {
        taskId,
        validId<TaskName>("kernel.persistence.async-task"),
        Value {Value::Object {{"input", Value {"blocking"}}}},
        validId<TraceId>("trace.document-close-task"),
        std::nullopt,
        project,
        document};
    auto submitDescriptor = lasercnc::test::taskSubmissionDescriptor(
        "command.document-close.task-submit", "task.document-close.submit");
    submitDescriptor.scope = ExecutionScope::Document;
    REQUIRE(lasercnc::test::registerAsyncCommand(
                kernel,
                std::move(submitDescriptor),
                std::make_shared<lasercnc::test::FixedTaskCommandHandler>(taskRequest))
                .hasValue());
    auto executor = BsThreadPoolExecutor::create(BsThreadPoolExecutorOptions {1U});
    REQUIRE(executor.hasValue());
    REQUIRE(kernel.configureTaskExecutor(std::move(executor).value()).hasValue());
    REQUIRE(kernel.bootstrap().hasValue());

    auto submitRequest = lasercnc::test::taskSubmissionRequest(
        "request.document-close.task-submit",
        "command.document-close.task-submit",
        session,
        "trace.document-close-task");
    submitRequest.context.projectId = project;
    submitRequest.context.documentId = document;
    auto accepted = kernel.execution().executeCommand(std::move(submitRequest));
    REQUIRE(accepted.hasValue());
    CHECK(accepted.value().taskId == taskId);
    enteredFuture.wait();

    auto refused = kernel.documentRuntime().close(document);
    REQUIRE_FALSE(refused.hasValue());
    CHECK(std::string(refused.error().code.value()) == "Document.CloseBlocked");
    auto lifecycle = kernel.documentRuntime().lifecycle(document);
    REQUIRE(lifecycle.hasValue());
    CHECK(lifecycle.value().state == DocumentLifecycleState::Open);

    release.set_value();
    auto terminal = kernel.execution().waitTask(taskId, std::chrono::seconds(2));
    REQUIRE(terminal.hasValue());
    CHECK(terminal.value().state == TaskState::Succeeded);
    REQUIRE(kernel.documentRuntime().close(document).hasValue());
    REQUIRE(kernel.shutdown().hasValue());
}

TEST_CASE("TransactionManager persists write-ahead journal before the memory swap", "[persistence][transaction]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    const auto projectId = validId<ProjectId>("project.write-ahead");
    const auto documentId = validId<DocumentId>("document.write-ahead");
    {
        DocumentStore documents;
        REQUIRE(documents.addDocument(projectId, documentId).hasValue());
        auto serializer = std::make_shared<ReentrantSerializer>(documents, documentId);
        auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(backend.hasValue());
        PersistenceService persistence;
        REQUIRE(persistence
                    .configure(
                        std::move(backend).value(),
                        serializer,
                        std::make_shared<Sha256HashService>())
                    .hasValue());
        REQUIRE(persistence.initialize().hasValue());
        TransactionManager transactions(documents, &persistence);
        auto transaction = transactions.begin(
            validId<TransactionId>("transaction.write-ahead"), documentId);
        REQUIRE(transaction.hasValue());
        REQUIRE(transaction.value()
                    ->createObject(ObjectRecord {
                        validId<ObjectId>("object.write-ahead"),
                        validId<ObjectTypeId>("kernel.persistence.test"),
                        Value {"persisted"}})
                    .hasValue());
        REQUIRE(transaction.value()->touchRevision(RevisionScope::Geometry).hasValue());
        auto committed = transaction.value()->commit();
        REQUIRE(committed.hasValue());
        CHECK(serializer->sawOldSnapshot);

        auto records = persistence.journalAfter(documentId, 0U);
        REQUIRE(records.hasValue());
        REQUIRE(records.value().size() == 1U);
        CHECK(records.value().front().transactionId
              == validId<TransactionId>("transaction.write-ahead"));
        auto snapshot = documents.snapshot(documentId);
        REQUIRE(snapshot.hasValue());
        CHECK(snapshot.value().objects().contains(
            validId<ObjectId>("object.write-ahead")));
        CHECK(snapshot.value().revisions().at(RevisionScope::Document) == Revision {1U});
    }
    removeDatabase(path);
}

TEST_CASE("TransactionManager leaves memory unchanged when journaling fails", "[persistence][transaction]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    const auto projectId = validId<ProjectId>("project.journal-failure");
    const auto documentId = validId<DocumentId>("document.journal-failure");
    {
        DocumentStore documents;
        REQUIRE(documents.addDocument(projectId, documentId).hasValue());
        auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(backend.hasValue());
        PersistenceService persistence;
        REQUIRE(persistence
                    .configure(
                        std::move(backend).value(),
                        std::make_shared<FailingSerializer>(),
                        std::make_shared<Sha256HashService>())
                    .hasValue());
        REQUIRE(persistence.initialize().hasValue());
        TransactionManager transactions(documents, &persistence);
        auto transaction = transactions.begin(
            validId<TransactionId>("transaction.journal-failure"), documentId);
        REQUIRE(transaction.hasValue());
        REQUIRE(transaction.value()
                    ->createObject(ObjectRecord {
                        validId<ObjectId>("object.must-not-commit"),
                        validId<ObjectTypeId>("kernel.persistence.test"),
                        Value {"unsafe"}})
                    .hasValue());
        auto committed = transaction.value()->commit();
        REQUIRE_FALSE(committed.hasValue());
        CHECK(std::string(committed.error().code.value()) == "Test.SerializeFailed");

        auto snapshot = documents.snapshot(documentId);
        REQUIRE(snapshot.hasValue());
        CHECK(snapshot.value().objects().empty());
        CHECK(snapshot.value().revisions().at(RevisionScope::Document) == Revision {0U});
        CHECK(persistence.journalAfter(documentId, 0U).value().empty());
    }
    removeDatabase(path);
}

TEST_CASE("AppKernel initializes and freezes configured persistence", "[kernel][persistence]")
{
    const auto path = uniqueDatabasePath();
    const auto latePath = uniqueDatabasePath();
    removeDatabase(path);
    removeDatabase(latePath);
    {
        lasercnc::kernel::AppKernel kernel;
        auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(backend.hasValue());
        REQUIRE(kernel.persistence()
                    .configure(
                        std::move(backend).value(),
                        std::make_shared<JsonconsAdapter>(),
                        std::make_shared<Sha256HashService>())
                    .hasValue());
        REQUIRE(kernel.bootstrap().hasValue());
        CHECK(kernel.persistence().ready());
        CHECK(kernel.persistence().frozen());

        auto lateBackend = SqlitePersistenceBackend::open(
            SqliteConnectionOptions {latePath});
        REQUIRE(lateBackend.hasValue());
        auto late = kernel.persistence().configure(
            std::move(lateBackend).value(),
            std::make_shared<JsonconsAdapter>(),
            std::make_shared<Sha256HashService>());
        REQUIRE_FALSE(late.hasValue());
        CHECK(std::string(late.error().code.value())
              == "Persistence.ConfigurationFrozen");
        REQUIRE(kernel.shutdown().hasValue());
    }
    removeDatabase(path);
    removeDatabase(latePath);
}

TEST_CASE("Workflow checkpoints round trip and fail closed on step tampering", "[persistence][workflow]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    const auto request = persistentWorkflowRequest("workflow.checkpoint-roundtrip");
    const auto definition = persistentWorkflowDefinition(false);
    {
        PersistenceService persistence;
        configureService(persistence, path);
        const auto snapshot = persistentWorkflowSnapshot(
            request, WorkflowState::Pending, WorkflowStepState::Pending);
        REQUIRE(persistence
                    .saveWorkflowCheckpoint(request, definition, snapshot, {})
                    .hasValue());
        auto loaded = persistence.workflowCheckpoint(request.workflowId);
        REQUIRE(loaded.hasValue());
        REQUIRE(loaded.value().has_value());
        CHECK(loaded.value()->request.workflowId == request.workflowId);
        CHECK(loaded.value()->snapshot.state == WorkflowState::Pending);
        CHECK(loaded.value()->snapshot.steps.size() == 1U);
        auto digest = persistence.workflowDefinitionDigest(definition);
        REQUIRE(digest.hasValue());
        CHECK(loaded.value()->definitionDigest == digest.value());

        auto completed = snapshot;
        completed.state = WorkflowState::Succeeded;
        completed.steps[0].state = WorkflowStepState::Succeeded;
        completed.steps[0].attempt = 1U;
        completed.steps[0].result = Value {Value::Object {{"checkpoint", Value {true}}}};
        completed.result = Value {Value::Object {}};
        REQUIRE(persistence
                    .saveWorkflowCheckpoint(
                        request,
                        definition,
                        completed,
                        {validId<WorkflowStepId>("step.persistence")})
                    .hasValue());
        auto all = persistence.workflowCheckpoints();
        REQUIRE(all.hasValue());
        REQUIRE(all.value().size() == 1U);
        CHECK(all.value()[0].snapshot.state == WorkflowState::Succeeded);
        REQUIRE(all.value()[0].completionOrder.size() == 1U);
    }
    {
        auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(backend.hasValue());
        const std::array parameters {
            Value {"{}"}, Value {std::string(request.workflowId.value())}};
        REQUIRE(backend.value()
                    ->execute(
                        "UPDATE workflow_steps SET payload=? WHERE workflow_id=?",
                        parameters)
                    .hasValue());
    }
    {
        PersistenceService reopened;
        configureService(reopened, path);
        auto corrupted = reopened.workflowCheckpoint(request.workflowId);
        REQUIRE_FALSE(corrupted.hasValue());
        CHECK(std::string(corrupted.error().code.value())
              == "Persistence.WorkflowStepDigestMismatch");
    }
    removeDatabase(path);
}

TEST_CASE("Workflow checkpoint errors preserve bounded cause chains and legacy leaves", "[persistence][workflow][error-chain]")
{
    const auto depth = GENERATE(1U, 32U);
    const auto path = uniqueDatabasePath();
    const auto request = persistentWorkflowRequest("workflow.error-chain");
    const auto definition = persistentWorkflowDefinition(false);
    std::shared_ptr<const Error> cause;
    for(unsigned int index = 0U; index < depth; ++index) {
        cause = std::make_shared<const Error>(makeError("Test.Cause" + std::to_string(index),
            ErrorCategory::Infrastructure, "durable diagnostic", Value{std::to_string(index)}, Severity::Warning, cause));
    }
    auto snapshot = persistentWorkflowSnapshot(request, WorkflowState::Failed, WorkflowStepState::Failed, 1U);
    snapshot.error = *cause;
    snapshot.steps.front().error = *cause;
    snapshot.compensationErrors = {*cause};
    {
        PersistenceService persistence;
        configureService(persistence, path);
        REQUIRE(persistence.saveWorkflowCheckpoint(request, definition, snapshot, {}).hasValue());
    }
    {
        PersistenceService reopened;
        configureService(reopened, path);
        const auto checkpoint = reopened.workflowCheckpoint(request.workflowId);
        REQUIRE(checkpoint.hasValue());
        REQUIRE(checkpoint.value().has_value());
        const auto& restored = checkpoint.value()->snapshot;
        REQUIRE(restored.error.has_value());
        REQUIRE(restored.steps.front().error.has_value());
        REQUIRE(restored.compensationErrors.size() == 1U);
        for(const auto* error : {&*restored.error, &*restored.steps.front().error, &restored.compensationErrors.front()}) {
            auto remaining = depth;
            for(auto current = error; current; current = current->cause.get()) {
                REQUIRE(remaining > 0U);
                --remaining;
                CHECK(std::string(current->code.value()) == "Test.Cause" + std::to_string(remaining));
                CHECK(current->category == ErrorCategory::Infrastructure);
                CHECK(current->severity == Severity::Warning);
                CHECK(current->message == "durable diagnostic");
                CHECK(current->details == Value{std::to_string(remaining)});
            }
            CHECK(remaining == 0U);
        }
        auto oversized = cause;
        for(auto index = depth; index < 33U; ++index) {
            oversized = std::make_shared<const Error>(makeError("Test.TooDeep", ErrorCategory::Internal,
                "too deep", Value{}, Severity::Error, oversized));
        }
        auto rejected = snapshot;
        rejected.error = *oversized;
        const auto saved = reopened.saveWorkflowCheckpoint(request, definition, rejected, {});
        REQUIRE_FALSE(saved.hasValue());
        CHECK(std::string(saved.error().code.value()) == "Persistence.SaveWorkflowFailed");
        const auto retained = reopened.workflowCheckpoint(request.workflowId);
        REQUIRE(retained.hasValue());
        REQUIRE(retained.value().has_value());
        REQUIRE(retained.value()->snapshot.error.has_value());
        CHECK(retained.value()->snapshot.error->code == cause->code);
        auto raw = SqlitePersistenceBackend::open({path});
        REQUIRE(raw.hasValue());
        const auto rows = raw.value()->query("SELECT payload FROM workflow_instances");
        REQUIRE(rows.hasValue());
        REQUIRE(rows.value().size() == 1U);
        const auto& payload = *rows.value().front().at("payload").getIf<std::string>();
        if(depth == 1U) { CHECK(payload.find("\"cause\"") == std::string::npos); }
    }
    removeDatabase(path);
}

TEST_CASE("Workflow checkpoint rejects malformed and overdeep persisted causes", "[persistence][workflow][error-chain]")
{
    const auto malformed = GENERATE(true, false);
    const auto path = uniqueDatabasePath();
    const auto request = persistentWorkflowRequest("workflow.invalid-cause");
    const auto definition = persistentWorkflowDefinition(false);
    {
        PersistenceService persistence;
        configureService(persistence, path);
        auto snapshot = persistentWorkflowSnapshot(request, WorkflowState::Failed, WorkflowStepState::Failed, 1U);
        snapshot.error = makeError("Test.Failed", ErrorCategory::Internal, "expected failure");
        REQUIRE(persistence.saveWorkflowCheckpoint(request, definition, snapshot, {}).hasValue());
        auto raw = SqlitePersistenceBackend::open({path});
        REQUIRE(raw.hasValue());
        auto rows = raw.value()->query("SELECT payload FROM workflow_instances");
        REQUIRE(rows.hasValue());
        REQUIRE(rows.value().size() == 1U);
        JsonconsAdapter json;
        auto payload = json.deserialize(*rows.value().front().at("payload").getIf<std::string>());
        REQUIRE(payload.hasValue());
        auto& error = payload.value().getIf<Value::Object>()->at("error");
        if(malformed) {
            error.getIf<Value::Object>()->insert_or_assign("cause", Value{"invalid cause"});
        } else {
            const auto leaf = error;
            for(unsigned int index = 1U; index < 33U; ++index) {
                auto outer = leaf;
                outer.getIf<Value::Object>()->insert_or_assign("cause", std::move(error));
                error = std::move(outer);
            }
        }
        auto encoded = json.serialize(payload.value());
        REQUIRE(encoded.hasValue());
        Sha256HashService hashes;
        auto digest = hashes.digest({reinterpret_cast<const std::byte*>(encoded.value().data()), encoded.value().size()});
        REQUIRE(digest.hasValue());
        const std::array parameters{Value{encoded.value()}, Value{std::string(digest.value().value())}};
        REQUIRE(raw.value()->execute("UPDATE workflow_instances SET payload=?,digest=?", parameters).hasValue());
        const auto loaded = persistence.workflowCheckpoint(request.workflowId);
        REQUIRE_FALSE(loaded.hasValue());
        CHECK(std::string(loaded.error().code.value()) == "Persistence.InvalidWorkflowPayload");
    }
    removeDatabase(path);
}

TEST_CASE("AppKernel restores a running workflow at the same idempotent attempt", "[persistence][workflow][recovery]")
{
    const auto path = uniqueDatabasePath();
    const auto snapshotDirectory = uniqueSnapshotDirectory();
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
    const auto request = persistentWorkflowRequest("workflow.running-recovery");
    const auto definition = persistentWorkflowDefinition(true);
    {
        PersistenceService seed;
        configureService(seed, path);
        auto running = persistentWorkflowSnapshot(
            request, WorkflowState::Running, WorkflowStepState::Running, 1U);
        REQUIRE(seed.saveWorkflowCheckpoint(request, definition, running, {}).hasValue());
    }
    {
        lasercnc::kernel::AppKernel kernel;
        auto handler = std::make_shared<PersistentCreateHandler>();
        configureRuntimeKernel(
            kernel,
            path,
            snapshotDirectory,
            request.sessionId,
            handler);
        REQUIRE(kernel.addDocument(request.projectId, request.documentId).hasValue());
        REQUIRE(lasercnc::test::registerWorkflow(kernel, definition).hasValue());
        REQUIRE(kernel.bootstrap().hasValue());

        auto recovered = kernel.execution().workflow(request.workflowId);
        REQUIRE(recovered.hasValue());
        CHECK(recovered.value().state == WorkflowState::Waiting);
        REQUIRE(recovered.value().steps.size() == 1U);
        CHECK(recovered.value().steps[0].state == WorkflowStepState::Waiting);
        CHECK(recovered.value().steps[0].attempt == 1U);
        CHECK(recovered.value().steps[0].replayCurrentAttempt);

        auto completed = kernel.execution().advanceWorkflow(request.workflowId);
        REQUIRE(completed.hasValue());
        CHECK(completed.value().state == WorkflowState::Succeeded);
        CHECK(completed.value().steps[0].attempt == 1U);
        CHECK(handler->calls == 1U);
        REQUIRE(kernel.shutdown().hasValue());
    }
    {
        lasercnc::kernel::AppKernel kernel;
        auto handler = std::make_shared<PersistentCreateHandler>();
        configureRuntimeKernel(
            kernel,
            path,
            snapshotDirectory,
            request.sessionId,
            handler);
        REQUIRE(kernel.addDocument(request.projectId, request.documentId).hasValue());
        REQUIRE(lasercnc::test::registerWorkflow(kernel, definition).hasValue());
        REQUIRE(kernel.bootstrap().hasValue());
        auto recovered = kernel.execution().workflow(request.workflowId);
        REQUIRE(recovered.hasValue());
        CHECK(recovered.value().state == WorkflowState::Succeeded);
        CHECK(handler->calls == 0U);
        REQUIRE(kernel.shutdown().hasValue());
    }
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
}

TEST_CASE("Workflow checkpoint failure prevents handler execution", "[persistence][workflow][failure]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    const auto request = persistentWorkflowRequest("workflow.checkpoint-failure");
    const auto definition = persistentWorkflowDefinition(true);
    {
        lasercnc::kernel::AppKernel kernel;
        REQUIRE(lasercnc::test::registerObjectType(kernel,
            lasercnc::test::valueObjectType("kernel.persistence.command")).hasValue());
        auto adapter = std::make_shared<JsonconsAdapter>();
        REQUIRE(kernel.executionServices()
                    .configure(adapter, std::make_shared<NullLogService>())
                    .hasValue());
        REQUIRE(kernel.capabilities()
                    .replace(
                        request.sessionId,
                        std::array {validId<CapabilityId>("document.write")})
                    .hasValue());
        auto handler = std::make_shared<PersistentCreateHandler>();
        REQUIRE(lasercnc::test::registerCommand(kernel, persistentCommandDescriptor(), handler)
                    .hasValue());
        REQUIRE(lasercnc::test::registerWorkflow(kernel, definition).hasValue());
        REQUIRE(kernel.addDocument(request.projectId, request.documentId).hasValue());
        auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(backend.hasValue());
        auto failing = std::make_unique<FailingWorkflowCheckpointBackend>(
            std::move(backend).value());
        auto* observed = failing.get();
        REQUIRE(kernel.persistence()
                    .configure(
                        std::move(failing),
                        adapter,
                        std::make_shared<Sha256HashService>())
                    .hasValue());
        REQUIRE(kernel.bootstrap().hasValue());
        REQUIRE(kernel.execution().startWorkflow(request).hasValue());

        observed->failCheckpoint.store(true, std::memory_order_release);
        auto failed = kernel.execution().advanceWorkflow(request.workflowId);
        REQUIRE_FALSE(failed.hasValue());
        CHECK(std::string(failed.error().code.value())
              == "Test.WorkflowCheckpointFailed");
        CHECK(handler->calls == 0U);
        auto unchanged = kernel.execution().workflow(request.workflowId);
        REQUIRE(unchanged.hasValue());
        CHECK(unchanged.value().steps[0].state == WorkflowStepState::Pending);
        CHECK(unchanged.value().steps[0].attempt == 0U);

        observed->failCheckpoint.store(false, std::memory_order_release);
        auto completed = kernel.execution().advanceWorkflow(request.workflowId);
        REQUIRE(completed.hasValue());
        CHECK(completed.value().state == WorkflowState::Succeeded);
        CHECK(handler->calls == 1U);
        REQUIRE(kernel.shutdown().hasValue());
    }
    removeDatabase(path);
}

TEST_CASE("Workflow post-command checkpoint faults preserve one durable command across restart", "[workflow][persistence][fault-matrix]")
{
    using namespace lasercnc::test;
    const auto retryBeforeRestart = GENERATE(false, true);
    for(const std::string stage : {"instance-upsert", "step-delete", "step-insert", "commit", "instance-hash", "step-hash"}) {
        for(const bool throws : {false, true}) {
            DYNAMIC_SECTION(stage << " throws=" << throws << " retryBeforeRestart=" << retryBeforeRestart) {
                const auto path = uniqueDatabasePath();
                const auto workflow = persistentWorkflowRequest("workflow.post-command-fault");
                const auto definition = persistentWorkflowDefinition(true);
                const auto setup = [&](AppKernel& kernel, const std::shared_ptr<PersistentCreateHandler>& handler,
                    std::unique_ptr<lasercnc::platform::IPersistenceBackend> backend,
                    std::shared_ptr<lasercnc::platform::IHashService> hashes, bool add) {
                    REQUIRE(registerObjectType(kernel, valueObjectType("kernel.persistence.command")).hasValue());
                    REQUIRE(kernel.executionServices().configure(std::make_shared<JsonconsAdapter>(),
                        std::make_shared<NullLogService>()).hasValue());
                    REQUIRE(kernel.capabilities().replace(workflow.sessionId,
                        std::array{validId<CapabilityId>("document.write")}).hasValue());
                    REQUIRE(registerCommand(kernel, persistentCommandDescriptor(), handler).hasValue());
                    REQUIRE(registerWorkflow(kernel, definition).hasValue());
                    if(add) { REQUIRE(kernel.addDocument(workflow.projectId, workflow.documentId).hasValue()); }
                    REQUIRE(kernel.persistence().configure(std::move(backend), std::make_shared<JsonconsAdapter>(),
                        std::move(hashes)).hasValue());
                    REQUIRE(kernel.bootstrap().hasValue());
                };
                {
                    AppKernel kernel;
                    auto sqlite = SqlitePersistenceBackend::open({path});
                    REQUIRE(sqlite.hasValue());
                    auto backend = std::make_unique<FaultInjectingBackend>(std::move(sqlite).value());
                    auto* db = backend.get();
                    auto hashes = std::make_shared<FaultHashService>(std::make_shared<Sha256HashService>());
                    auto handler = std::make_shared<PersistentCreateHandler>();
                    setup(kernel, handler, std::move(backend), hashes, true);
                    REQUIRE(kernel.execution().startWorkflow(workflow).hasValue());
                    if(stage == "instance-upsert") { db->arm(BackendPoint::Execute, "INSERT INTO workflow_instances", 2U, throws); }
                    if(stage == "step-delete") { db->arm(BackendPoint::Execute, "DELETE FROM workflow_steps", 2U, throws); }
                    if(stage == "step-insert") { db->arm(BackendPoint::Execute, "INSERT INTO workflow_steps", 2U, throws); }
                    if(stage == "commit") { db->arm(BackendPoint::Commit, "INSERT INTO workflow_instances", 2U, throws); }
                    if(stage == "instance-hash") { hashes->arm("lasercnc.workflow-checkpoint", 2U, throws); }
                    if(stage == "step-hash") { hashes->arm("lasercnc.workflow-step-checkpoint", 2U, throws, "lasercnc.workflow-checkpoint"); }
                    auto failed = kernel.execution().advanceWorkflow(workflow.workflowId);
                    REQUIRE_FALSE(failed.hasValue());
                    CHECK(db->hits + hashes->hits == 1U);
                    CHECK(handler->calls == 1U);
                    const auto memory = kernel.execution().workflow(workflow.workflowId).value();
                    CHECK(memory.steps.front().state == WorkflowStepState::Succeeded);
                    CHECK(memory.steps.front().attempt == 1U);
                    const auto durable = kernel.persistence().workflowCheckpoint(workflow.workflowId);
                    REQUIRE(durable.hasValue());
                    REQUIRE(durable.value().has_value());
                    CHECK(durable.value()->snapshot.steps.front().state == WorkflowStepState::Running);
                    CHECK(durable.value()->snapshot.steps.front().attempt == 1U);
                    CHECK(kernel.persistence().journalAfter(workflow.documentId, 0U).value().size() == 1U);
                    CHECK(kernel.documents().snapshot(workflow.documentId).value().objects().size() == 1U);
                    if(retryBeforeRestart) {
                        auto flushed = kernel.execution().advanceWorkflow(workflow.workflowId);
                        REQUIRE(flushed.hasValue());
                        CHECK(flushed.value().state == WorkflowState::Succeeded);
                        CHECK(handler->calls == 1U);
                    }
                    REQUIRE(kernel.shutdown().hasValue());
                }
                {
                    AppKernel kernel;
                    auto sqlite = SqlitePersistenceBackend::open({path});
                    REQUIRE(sqlite.hasValue());
                    auto handler = std::make_shared<PersistentCreateHandler>();
                    setup(kernel, handler, std::move(sqlite).value(), std::make_shared<Sha256HashService>(), false);
                    auto recovered = kernel.execution().advanceWorkflow(workflow.workflowId);
                    REQUIRE(recovered.hasValue());
                    CHECK(recovered.value().state == WorkflowState::Succeeded);
                    CHECK(recovered.value().steps.front().attempt == 1U);
                    CHECK(handler->calls == 0U);
                    CHECK(kernel.persistence().journalAfter(workflow.documentId, 0U).value().size() == 1U);
                    CHECK(kernel.documents().snapshot(workflow.documentId).value().revisions().at(RevisionScope::Document) == Revision{1U});
                    REQUIRE(kernel.shutdown().hasValue());
                }
                removeDatabase(path);
            }
        }
    }
}

TEST_CASE("AppKernel rejects durable workflow definition drift", "[persistence][workflow][recovery]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    const auto request = persistentWorkflowRequest("workflow.definition-drift");
    auto original = persistentWorkflowDefinition(false);
    {
        PersistenceService seed;
        configureService(seed, path);
        REQUIRE(seed
                    .saveWorkflowCheckpoint(
                        request,
                        original,
                        persistentWorkflowSnapshot(
                            request,
                            WorkflowState::Pending,
                            WorkflowStepState::Pending),
                        {})
                    .hasValue());
    }
    {
        lasercnc::kernel::AppKernel kernel;
        auto adapter = std::make_shared<JsonconsAdapter>();
        REQUIRE(kernel.executionServices()
                    .configure(adapter, std::make_shared<NullLogService>())
                    .hasValue());
        auto changed = persistentWorkflowDefinition(false);
        changed.steps[0].valueTemplate = Value {Value::Object {{"checkpoint", Value {false}}}};
        REQUIRE(lasercnc::test::registerWorkflow(kernel, std::move(changed)).hasValue());
        auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(backend.hasValue());
        REQUIRE(kernel.persistence()
                    .configure(
                        std::move(backend).value(),
                        adapter,
                        std::make_shared<Sha256HashService>())
                    .hasValue());
        auto bootstrapped = kernel.bootstrap();
        REQUIRE_FALSE(bootstrapped.hasValue());
        CHECK(std::string(bootstrapped.error().code.value())
              == "Workflow.KernelRecoveryFailed");
        REQUIRE(bootstrapped.error().cause != nullptr);
        CHECK(std::string(bootstrapped.error().cause->code.value())
              == "Workflow.RecoveryDefinitionMismatch");
        CHECK(kernel.state() == AppKernelState::Failed);
    }
    removeDatabase(path);
}

TEST_CASE("External-effect recovery never replays executing work automatically", "[persistence][effect][recovery]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    const Value signature {Value::Object {
        {"command", Value {"kernel.effect.test"}},
        {"version", Value {"1.0.0"}}}};
    const auto safeKey = validId<IdempotencyKey>("effect.safe");
    const auto idempotentKey = validId<IdempotencyKey>("effect.idempotent");
    const auto reconcileKey = validId<IdempotencyKey>("effect.reconcile");
    const auto neverKey = validId<IdempotencyKey>("effect.never");

    {
        PersistenceService persistence;
        configureService(persistence, path);
        REQUIRE(persistence.claimExternalEffect(
            safeKey, signature, ReplayPolicy::Safe).hasValue());
        REQUIRE(persistence.claimExternalEffect(
            idempotentKey, signature, ReplayPolicy::Idempotent).hasValue());
        REQUIRE(persistence.claimExternalEffect(
            reconcileKey, signature, ReplayPolicy::ReconcileOnly).hasValue());
        REQUIRE(persistence.claimExternalEffect(
            neverKey, signature, ReplayPolicy::Never).hasValue());
        CHECK(persistence.externalEffect(safeKey).value()->state
              == ExternalEffectState::Executing);
    }

    {
        PersistenceService persistence;
        configureService(persistence, path);
        REQUIRE(persistence.externalEffect(safeKey).hasValue());
        CHECK(persistence.externalEffect(safeKey).value()->state
              == ExternalEffectState::Interrupted);
        CHECK(persistence.externalEffect(idempotentKey).value()->state
              == ExternalEffectState::Interrupted);
        CHECK(persistence.externalEffect(reconcileKey).value()->state
              == ExternalEffectState::ReconcileRequired);
        CHECK(persistence.externalEffect(neverKey).value()->state
              == ExternalEffectState::Indeterminate);

        auto resumed = persistence.claimExternalEffect(
            safeKey, signature, ReplayPolicy::Safe);
        REQUIRE(resumed.hasValue());
        CHECK(resumed.value().disposition == ExternalEffectClaimDisposition::Acquired);
        CHECK(resumed.value().resumed);
        const Value outcome {Value::Object {{"published", Value {true}}}};
        REQUIRE(persistence.completeExternalEffect(safeKey, signature, outcome).hasValue());
        auto replayed = persistence.claimExternalEffect(
            safeKey, signature, ReplayPolicy::Safe);
        REQUIRE(replayed.hasValue());
        CHECK(replayed.value().disposition == ExternalEffectClaimDisposition::Replayed);
        REQUIRE(replayed.value().replay.has_value());
        CHECK(*replayed.value().replay == outcome);

        auto idempotentRetry = persistence.claimExternalEffect(
            idempotentKey, signature, ReplayPolicy::Idempotent);
        REQUIRE(idempotentRetry.hasValue());
        CHECK(idempotentRetry.value().resumed);
        auto interrupted = persistence.interruptExternalEffect(idempotentKey, signature);
        REQUIRE(interrupted.hasValue());
        CHECK(interrupted.value() == RecoveryDisposition::Interrupted);

        auto reconcile = persistence.claimExternalEffect(
            reconcileKey, signature, ReplayPolicy::ReconcileOnly);
        REQUIRE_FALSE(reconcile.hasValue());
        CHECK(std::string(reconcile.error().code.value())
              == "Persistence.ExternalEffectReconcileRequired");
        auto never = persistence.claimExternalEffect(
            neverKey, signature, ReplayPolicy::Never);
        REQUIRE_FALSE(never.hasValue());
        CHECK(std::string(never.error().code.value())
              == "Persistence.ExternalEffectIndeterminate");

        const Value changedSignature {Value::Object {
            {"command", Value {"kernel.effect.changed"}}}};
        auto conflict = persistence.claimExternalEffect(
            safeKey, changedSignature, ReplayPolicy::Safe);
        REQUIRE_FALSE(conflict.hasValue());
        CHECK(std::string(conflict.error().code.value())
              == "Persistence.ExternalEffectIdentityConflict");

        auto tamper = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(tamper.hasValue());
        const std::array tamperParameters {
            Value {"never"}, Value {std::string(idempotentKey.value())}};
        REQUIRE(tamper.value()->execute(
            "UPDATE external_effects SET replay_policy=? WHERE idempotency_key=?",
            tamperParameters).hasValue());
        auto corrupted = persistence.externalEffect(idempotentKey);
        REQUIRE_FALSE(corrupted.hasValue());
        CHECK(std::string(corrupted.error().code.value())
              == "Persistence.ExternalEffectStatePolicyMismatch");
    }
    removeDatabase(path);
}

TEST_CASE("CommandRuntime persists and replays completed external effects", "[runtime][effect][persistence]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    const auto session = validId<SessionId>("session.effect-completed");
    const auto key = validId<IdempotencyKey>("effect.completed-replay");
    constexpr auto command = "kernel.effect.publish";
    {
        lasercnc::kernel::AppKernel kernel;
        auto adapter = std::make_shared<JsonconsAdapter>();
        REQUIRE(kernel.executionServices()
                    .configure(adapter, std::make_shared<NullLogService>())
                    .hasValue());
        auto handler = std::make_shared<TestEffectHandler>();
        auto guard = std::make_shared<TestEffectGuard>();
        REQUIRE(kernel.effectGuards().registerGuard(
            validId<EffectGuardId>("guard.effect.publish"), guard).hasValue());
        REQUIRE(lasercnc::test::registerExternalEffectCommand(kernel,
            externalEffectDescriptor(command, ReplayPolicy::Idempotent), handler).hasValue());
        const std::array grants {validId<CapabilityId>("effect.publish")};
        REQUIRE(kernel.capabilities().replace(session, grants).hasValue());
        REQUIRE(kernel.resources().configure(
            ResourceKind::DiskIO,
            validId<ResourceId>("resource.effect.publish"),
            1U).hasValue());
        auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(backend.hasValue());
        REQUIRE(kernel.persistence().configure(
            std::move(backend).value(),
            adapter,
            std::make_shared<Sha256HashService>()).hasValue());
        REQUIRE(kernel.bootstrap().hasValue());

        auto first = kernel.execution().executeCommand(
            externalEffectRequest("request.effect.first", command, session, key));
        REQUIRE(first.hasValue());
        CHECK_FALSE(first.value().replayed);
        REQUIRE(first.value().recoveryDisposition.has_value());
        CHECK(*first.value().recoveryDisposition == RecoveryDisposition::Completed);
        CHECK(handler->calls == 1U);
        auto replay = kernel.execution().executeCommand(
            externalEffectRequest("request.effect.replay", command, session, key));
        REQUIRE(replay.hasValue());
        CHECK(replay.value().replayed);
        CHECK(handler->calls == 1U);
        CHECK(guard->calls == 2U);
        auto record = kernel.persistence().externalEffect(key);
        REQUIRE(record.hasValue());
        REQUIRE(record.value().has_value());
        CHECK(record.value()->state == ExternalEffectState::Completed);
        REQUIRE(kernel.shutdown().hasValue());
    }
    {
        lasercnc::kernel::AppKernel kernel;
        auto adapter = std::make_shared<JsonconsAdapter>();
        REQUIRE(kernel.executionServices()
                    .configure(adapter, std::make_shared<NullLogService>())
                    .hasValue());
        auto handler = std::make_shared<TestEffectHandler>();
        REQUIRE(kernel.effectGuards().registerGuard(
            validId<EffectGuardId>("guard.effect.publish"),
            std::make_shared<TestEffectGuard>()).hasValue());
        REQUIRE(lasercnc::test::registerExternalEffectCommand(kernel,
            externalEffectDescriptor(command, ReplayPolicy::Idempotent), handler).hasValue());
        const std::array grants {validId<CapabilityId>("effect.publish")};
        REQUIRE(kernel.capabilities().replace(session, grants).hasValue());
        auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(backend.hasValue());
        REQUIRE(kernel.persistence().configure(
            std::move(backend).value(),
            adapter,
            std::make_shared<Sha256HashService>()).hasValue());
        REQUIRE(kernel.bootstrap().hasValue());
        auto replay = kernel.execution().executeCommand(
            externalEffectRequest("request.effect.restart", command, session, key));
        REQUIRE(replay.hasValue());
        CHECK(replay.value().replayed);
        CHECK(handler->calls == 0U);
        REQUIRE(kernel.shutdown().hasValue());
    }
    removeDatabase(path);
}

TEST_CASE("Safe external effects require explicit same-key retry after failure", "[runtime][effect][recovery]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    const auto session = validId<SessionId>("session.effect-retry");
    const auto key = validId<IdempotencyKey>("effect.explicit-retry");
    constexpr auto command = "kernel.effect.safe-publish";
    lasercnc::kernel::AppKernel kernel;
    auto adapter = std::make_shared<JsonconsAdapter>();
    REQUIRE(kernel.executionServices()
                .configure(adapter, std::make_shared<NullLogService>())
                .hasValue());
    auto handler = std::make_shared<TestEffectHandler>();
    handler->fail.store(true, std::memory_order_release);
    auto guard = std::make_shared<TestEffectGuard>();
    REQUIRE(kernel.effectGuards().registerGuard(
        validId<EffectGuardId>("guard.effect.publish"), guard).hasValue());
    REQUIRE(lasercnc::test::registerExternalEffectCommand(kernel,
        externalEffectDescriptor(command, ReplayPolicy::Safe), handler).hasValue());
    const std::array grants {validId<CapabilityId>("effect.publish")};
    REQUIRE(kernel.capabilities().replace(session, grants).hasValue());
    auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
    REQUIRE(backend.hasValue());
    REQUIRE(kernel.persistence().configure(
        std::move(backend).value(),
        adapter,
        std::make_shared<Sha256HashService>()).hasValue());
    REQUIRE(kernel.bootstrap().hasValue());

    auto failed = kernel.execution().executeCommand(
        externalEffectRequest("request.effect.failed", command, session, key));
    REQUIRE_FALSE(failed.hasValue());
    CHECK(std::string(failed.error().code.value()) == "Effect.ExecutionInterrupted");
    REQUIRE(failed.error().cause != nullptr);
    CHECK(std::string(failed.error().cause->code.value())
          == "Test.ExternalEffectFailed");
    CHECK(kernel.persistence().externalEffect(key).value()->state
          == ExternalEffectState::Interrupted);

    handler->fail.store(false, std::memory_order_release);
    auto retried = kernel.execution().executeCommand(
        externalEffectRequest("request.effect.retry", command, session, key));
    REQUIRE(retried.hasValue());
    CHECK_FALSE(retried.value().replayed);
    CHECK(handler->calls == 2U);
    CHECK(handler->resumed.load(std::memory_order_acquire));
    CHECK(kernel.persistence().externalEffect(key).value()->state
          == ExternalEffectState::Completed);
    REQUIRE(kernel.shutdown().hasValue());
    removeDatabase(path);
}

TEST_CASE("Effect guards fail before durable claim and handler execution", "[runtime][effect][guard]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    const auto session = validId<SessionId>("session.effect-guard");
    const auto deniedSession = validId<SessionId>("session.effect-no-capability");
    const auto key = validId<IdempotencyKey>("effect.guard-denied");
    const auto deniedKey = validId<IdempotencyKey>("effect.capability-denied");
    constexpr auto command = "kernel.effect.guarded";
    lasercnc::kernel::AppKernel kernel;
    auto adapter = std::make_shared<JsonconsAdapter>();
    REQUIRE(kernel.executionServices()
                .configure(adapter, std::make_shared<NullLogService>())
                .hasValue());
    auto handler = std::make_shared<TestEffectHandler>();
    auto guard = std::make_shared<TestEffectGuard>();
    guard->allow.store(false, std::memory_order_release);
    REQUIRE(kernel.effectGuards().registerGuard(
        validId<EffectGuardId>("guard.effect.publish"), guard).hasValue());
    REQUIRE(lasercnc::test::registerExternalEffectCommand(kernel,
        externalEffectDescriptor(command, ReplayPolicy::Never), handler).hasValue());
    const std::array grants {validId<CapabilityId>("effect.publish")};
    REQUIRE(kernel.capabilities().replace(session, grants).hasValue());
    auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
    REQUIRE(backend.hasValue());
    REQUIRE(kernel.persistence().configure(
        std::move(backend).value(),
        adapter,
        std::make_shared<Sha256HashService>()).hasValue());
    REQUIRE(kernel.bootstrap().hasValue());

    auto unauthorized = kernel.execution().executeCommand(externalEffectRequest(
        "request.effect.unauthorized", command, deniedSession, deniedKey));
    REQUIRE_FALSE(unauthorized.hasValue());
    CHECK(std::string(unauthorized.error().code.value()) == "Capability.Denied");
    CHECK(guard->calls == 0U);
    CHECK_FALSE(kernel.persistence().externalEffect(deniedKey).value().has_value());

    auto denied = kernel.execution().executeCommand(
        externalEffectRequest("request.effect.denied", command, session, key));
    REQUIRE_FALSE(denied.hasValue());
    CHECK(std::string(denied.error().code.value()) == "Test.EffectGuardDenied");
    CHECK(handler->calls == 0U);
    CHECK(guard->calls == 1U);
    auto record = kernel.persistence().externalEffect(key);
    REQUIRE(record.hasValue());
    CHECK_FALSE(record.value().has_value());
    REQUIRE(kernel.shutdown().hasValue());
    removeDatabase(path);
}

TEST_CASE("Unsafe external-effect failures become reconcile or indeterminate", "[runtime][effect][recovery]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    const auto session = validId<SessionId>("session.effect-unsafe");
    constexpr auto reconcileCommand = "kernel.effect.reconcile-only";
    constexpr auto neverCommand = "kernel.effect.never";
    const auto reconcileKey = validId<IdempotencyKey>("effect.runtime-reconcile");
    const auto neverKey = validId<IdempotencyKey>("effect.runtime-never");
    lasercnc::kernel::AppKernel kernel;
    auto adapter = std::make_shared<JsonconsAdapter>();
    REQUIRE(kernel.executionServices()
                .configure(adapter, std::make_shared<NullLogService>())
                .hasValue());
    auto handler = std::make_shared<TestEffectHandler>();
    handler->fail.store(true, std::memory_order_release);
    REQUIRE(kernel.effectGuards().registerGuard(
        validId<EffectGuardId>("guard.effect.publish"),
        std::make_shared<TestEffectGuard>()).hasValue());
    REQUIRE(lasercnc::test::registerExternalEffectCommand(kernel,
        externalEffectDescriptor(reconcileCommand, ReplayPolicy::ReconcileOnly),
        handler).hasValue());
    REQUIRE(lasercnc::test::registerExternalEffectCommand(kernel,
        externalEffectDescriptor(neverCommand, ReplayPolicy::Never), handler).hasValue());
    const std::array grants {validId<CapabilityId>("effect.publish")};
    REQUIRE(kernel.capabilities().replace(session, grants).hasValue());
    auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
    REQUIRE(backend.hasValue());
    REQUIRE(kernel.persistence().configure(
        std::move(backend).value(),
        adapter,
        std::make_shared<Sha256HashService>()).hasValue());
    REQUIRE(kernel.bootstrap().hasValue());

    auto reconcile = kernel.execution().executeCommand(externalEffectRequest(
        "request.effect.reconcile", reconcileCommand, session, reconcileKey));
    REQUIRE_FALSE(reconcile.hasValue());
    CHECK(std::string(reconcile.error().code.value()) == "Effect.ReconcileRequired");
    CHECK(kernel.persistence().externalEffect(reconcileKey).value()->state
          == ExternalEffectState::ReconcileRequired);
    auto reconcileRetry = kernel.execution().executeCommand(externalEffectRequest(
        "request.effect.reconcile-retry", reconcileCommand, session, reconcileKey));
    REQUIRE_FALSE(reconcileRetry.hasValue());
    CHECK(std::string(reconcileRetry.error().code.value()) == "Effect.DurableClaimFailed");
    REQUIRE(reconcileRetry.error().cause != nullptr);
    CHECK(std::string(reconcileRetry.error().cause->code.value())
          == "Persistence.ExternalEffectReconcileRequired");

    auto never = kernel.execution().executeCommand(externalEffectRequest(
        "request.effect.never", neverCommand, session, neverKey));
    REQUIRE_FALSE(never.hasValue());
    CHECK(std::string(never.error().code.value()) == "Effect.Indeterminate");
    CHECK(kernel.persistence().externalEffect(neverKey).value()->state
          == ExternalEffectState::Indeterminate);
    auto neverRetry = kernel.execution().executeCommand(externalEffectRequest(
        "request.effect.never-retry", neverCommand, session, neverKey));
    REQUIRE_FALSE(neverRetry.hasValue());
    CHECK(std::string(neverRetry.error().code.value()) == "Effect.DurableClaimFailed");
    REQUIRE(neverRetry.error().cause != nullptr);
    CHECK(std::string(neverRetry.error().cause->code.value())
          == "Persistence.ExternalEffectIndeterminate");
    CHECK(handler->calls == 2U);
    REQUIRE(kernel.shutdown().hasValue());
    removeDatabase(path);
}

TEST_CASE("External effects require registered guards and durable persistence at bootstrap", "[kernel][effect][guard]")
{
    constexpr auto command = "kernel.effect.bootstrap-validation";
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    {
        lasercnc::kernel::AppKernel kernel;
        auto adapter = std::make_shared<JsonconsAdapter>();
        REQUIRE(kernel.executionServices()
                    .configure(adapter, std::make_shared<NullLogService>())
                    .hasValue());
        REQUIRE(lasercnc::test::registerExternalEffectCommand(kernel,
            externalEffectDescriptor(command, ReplayPolicy::Never),
            std::make_shared<TestEffectHandler>()).hasValue());
        auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(backend.hasValue());
        REQUIRE(kernel.persistence().configure(
            std::move(backend).value(),
            adapter,
            std::make_shared<Sha256HashService>()).hasValue());
        auto bootstrapped = kernel.bootstrap();
        REQUIRE_FALSE(bootstrapped.hasValue());
        CHECK(std::string(bootstrapped.error().code.value())
              == "Effect.RegistryValidationFailed");
        REQUIRE(bootstrapped.error().cause != nullptr);
        CHECK(std::string(bootstrapped.error().cause->code.value())
              == "EffectGuard.CommandRequirementMissing");
    }
    removeDatabase(path);
    {
        lasercnc::kernel::AppKernel kernel;
        auto adapter = std::make_shared<JsonconsAdapter>();
        REQUIRE(kernel.executionServices()
                    .configure(adapter, std::make_shared<NullLogService>())
                    .hasValue());
        REQUIRE(kernel.effectGuards().registerGuard(
            validId<EffectGuardId>("guard.effect.publish"),
            std::make_shared<TestEffectGuard>()).hasValue());
        REQUIRE(lasercnc::test::registerExternalEffectCommand(kernel,
            externalEffectDescriptor(command, ReplayPolicy::Never),
            std::make_shared<TestEffectHandler>()).hasValue());
        auto bootstrapped = kernel.bootstrap();
        REQUIRE_FALSE(bootstrapped.hasValue());
        CHECK(std::string(bootstrapped.error().code.value())
              == "Effect.RegistryValidationFailed");
        REQUIRE(bootstrapped.error().cause != nullptr);
        CHECK(std::string(bootstrapped.error().cause->code.value())
              == "Effect.PersistenceRequired");
    }
}

TEST_CASE("External effects acquire guards before exclusive resources and durable claims", "[runtime][effect][resource]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    const auto session = validId<SessionId>("session.effect-resource");
    const auto firstKey = validId<IdempotencyKey>("effect.resource-first");
    const auto secondKey = validId<IdempotencyKey>("effect.resource-second");
    constexpr auto command = "kernel.effect.resource-order";
    lasercnc::kernel::AppKernel kernel;
    auto adapter = std::make_shared<JsonconsAdapter>();
    REQUIRE(kernel.executionServices()
                .configure(adapter, std::make_shared<NullLogService>())
                .hasValue());
    auto handler = std::make_shared<BlockingEffectHandler>();
    auto guard = std::make_shared<TestEffectGuard>();
    REQUIRE(kernel.effectGuards().registerGuard(
        validId<EffectGuardId>("guard.effect.publish"), guard).hasValue());
    REQUIRE(lasercnc::test::registerExternalEffectCommand(kernel,
        externalEffectDescriptor(command, ReplayPolicy::Idempotent), handler).hasValue());
    const std::array grants {validId<CapabilityId>("effect.publish")};
    REQUIRE(kernel.capabilities().replace(session, grants).hasValue());
    REQUIRE(kernel.resources().configure(
        ResourceKind::DiskIO,
        validId<ResourceId>("resource.effect.publish"),
        1U).hasValue());
    auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
    REQUIRE(backend.hasValue());
    REQUIRE(kernel.persistence().configure(
        std::move(backend).value(),
        adapter,
        std::make_shared<Sha256HashService>()).hasValue());
    REQUIRE(kernel.bootstrap().hasValue());

    std::optional<Result<CommandResponse>> firstResult;
    std::thread first([&]() {
        firstResult = kernel.execution().executeCommand(externalEffectRequest(
            "request.effect.resource-first", command, session, firstKey));
    });
    handler->waitUntilEntered();
    auto second = kernel.execution().executeCommand(externalEffectRequest(
        "request.effect.resource-second", command, session, secondKey));
    handler->release();
    first.join();
    REQUIRE_FALSE(second.hasValue());
    CHECK(std::string(second.error().code.value()) == "Effect.ResourceBusy");
    CHECK(handler->calls == 1U);
    CHECK(guard->calls == 2U);
    auto secondRecord = kernel.persistence().externalEffect(secondKey);
    REQUIRE(secondRecord.hasValue());
    CHECK_FALSE(secondRecord.value().has_value());

    REQUIRE(firstResult.has_value());
    REQUIRE(firstResult->hasValue());
    const auto availability = kernel.resources().snapshot();
    REQUIRE(availability.size() == 1U);
    CHECK_FALSE(availability.front().exclusivelyHeld);
    REQUIRE(kernel.shutdown().hasValue());
    removeDatabase(path);
}
