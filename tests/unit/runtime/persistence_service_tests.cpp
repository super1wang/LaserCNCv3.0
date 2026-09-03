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

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
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
            Value {*dataValue}});
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
        return Result<AsyncCommandPlan>::success(AsyncCommandPlan {
            TaskRequest {
                std::move(taskId).value(),
                validId<TaskName>("kernel.persistence.async-task"),
                Value {Value::Object {{"input", Value {"durable"}}}},
                command.traceId},
            Value {Value::Object {{"accepted", Value {true}}}}});
    }

    std::atomic_size_t calls{0U};
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

TaskDescriptor persistentTaskDescriptor()
{
    return TaskDescriptor {
        validId<TaskName>("kernel.persistence.async-task"),
        Version {1U, 0U, 0U},
        objectSchema("schema.persistence.async-task.input"),
        objectSchema("schema.persistence.async-task.result")};
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
        sessionId,
        projectId,
        documentId,
        validId<CommandName>("kernel.persistence.create"),
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
    std::shared_ptr<PersistentCreateHandler> handler)
{
    auto adapter = std::make_shared<JsonconsAdapter>();
    REQUIRE(kernel.executionServices()
                .configure(adapter, std::make_shared<NullLogService>())
                .hasValue());
    const std::array capabilities {validId<CapabilityId>("document.write")};
    REQUIRE(kernel.capabilities().replace(sessionId, capabilities).hasValue());
    REQUIRE(kernel.commandRegistry()
                .registerHandler(persistentCommandDescriptor(), std::move(handler))
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
    std::shared_ptr<PersistentTaskHandler> taskHandler)
{
    auto adapter = std::make_shared<JsonconsAdapter>();
    REQUIRE(kernel.executionServices()
                .configure(adapter, std::make_shared<NullLogService>())
                .hasValue());
    const std::array capabilities {validId<CapabilityId>("task.submit")};
    REQUIRE(kernel.capabilities().replace(sessionId, capabilities).hasValue());
    REQUIRE(kernel.commandRegistry()
                .registerAsyncHandler(
                    persistentAsyncCommandDescriptor(), std::move(commandHandler))
                .hasValue());
    REQUIRE(kernel.taskRegistry()
                .registerHandler(persistentTaskDescriptor(), std::move(taskHandler))
                .hasValue());
    auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {database});
    REQUIRE(backend.hasValue());
    REQUIRE(kernel.persistence()
                .configure(
                    std::move(backend).value(),
                    adapter,
                    std::make_shared<Sha256HashService>())
                .hasValue());
    auto executor = BsThreadPoolExecutor::create(BsThreadPoolExecutorOptions {1U});
    REQUIRE(executor.hasValue());
    REQUIRE(kernel.configureTaskExecutor(std::move(executor).value()).hasValue());
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
        sessionId,
        projectId,
        documentId,
        validId<CommandName>("kernel.persistence.async-command"),
        Value {Value::Object {{"input", Value {"durable"}}}},
        std::nullopt,
        validId<CorrelationId>("correlation.persistence.async"),
        validId<TraceId>("trace.persistence.async"),
        key,
        std::nullopt};
}

} // namespace

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
        auto subscription = kernel.events().subscribe(
            validId<SubscriptionId>("subscription.recovery"),
            lasercnc::messaging::EventFilter {
                lasercnc::messaging::EventKind::Domain,
                validId<EventName>("kernel.recovery.history")},
            lasercnc::messaging::DeliveryMode::Immediate,
            [&delivered](const lasercnc::messaging::EventEnvelope&) { ++delivered; });
        REQUIRE(subscription.hasValue());
        REQUIRE(kernel.bootstrap().hasValue());
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
        auto first = kernel.commands().execute(persistentCommandRequest(
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
        auto replay = kernel.commands().execute(persistentCommandRequest(
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

        auto conflict = kernel.commands().execute(persistentCommandRequest(
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
        auto accepted = kernel.commands().execute(persistentAsyncCommandRequest(
            "request.async-replay.first",
            projectId,
            documentId,
            sessionId,
            key));
        REQUIRE(accepted.hasValue());
        CHECK_FALSE(accepted.value().replayed);
        REQUIRE(accepted.value().taskId.has_value());
        acceptedTaskId = *accepted.value().taskId;
        auto terminal = kernel.tasks().wait(acceptedTaskId, std::chrono::seconds(2));
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
        auto replay = kernel.commands().execute(persistentAsyncCommandRequest(
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

        auto terminal = kernel.tasks().snapshot(acceptedTaskId);
        REQUIRE(terminal.hasValue());
        CHECK(terminal.value().state == TaskState::Succeeded);
        REQUIRE(terminal.value().result.has_value());
        CHECK(*terminal.value().result == Value {Value::Object {
            {"value", Value {"completed"}}}});
        REQUIRE(kernel.shutdown().hasValue());
    }
    removeDatabase(path);
}

TEST_CASE("Task completion exposes persistence failure without changing task outcome", "[persistence][task][failure]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    lasercnc::kernel::AppKernel kernel;
    auto adapter = std::make_shared<JsonconsAdapter>();
    REQUIRE(kernel.executionServices()
                .configure(adapter, std::make_shared<NullLogService>())
                .hasValue());
    auto taskHandler = std::make_shared<PersistentTaskHandler>();
    REQUIRE(kernel.taskRegistry()
                .registerHandler(persistentTaskDescriptor(), taskHandler)
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

    const auto request = TaskRequest {
        validId<TaskId>("task.persistence-failure"),
        validId<TaskName>("kernel.persistence.async-task"),
        Value {Value::Object {{"input", Value {"durable"}}}},
        validId<TraceId>("trace.persistence-failure")};
    REQUIRE(kernel.tasks().submit(request).hasValue());
    auto terminal = kernel.tasks().wait(request.taskId, std::chrono::seconds(2));
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
        const std::array parameters {Value {std::int64_t {6}}, Value {"future"}};
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
