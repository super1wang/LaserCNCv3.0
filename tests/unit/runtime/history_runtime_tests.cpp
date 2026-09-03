#include <lasercnc/infrastructure/jsoncons_adapter.hpp>
#include <lasercnc/infrastructure/sha256_hash_service.hpp>
#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>
#include <lasercnc/kernel/app_kernel.hpp>
#include <lasercnc/observability/log_service.hpp>

#include <catch2/catch_test_macros.hpp>
#include "kernel_test_module.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace lasercnc::foundation;
using namespace lasercnc::infrastructure;
using namespace lasercnc::kernel;
using namespace lasercnc::observability;
using namespace lasercnc::runtime;
using namespace lasercnc::state;

namespace {

template <typename Id>
Id validId(const char* value)
{
    auto created = Id::create(value);
    if(!created) {
        throw std::logic_error("Invalid test identity");
    }
    return std::move(created).value();
}

Schema schema(const char* id)
{
    auto created = Schema::create(
        validId<SchemaId>(id), Version {1U, 0U, 0U}, SchemaKind::Object);
    if(!created) {
        throw std::logic_error("Invalid test schema");
    }
    return std::move(created).value();
}

class NullLog final : public ILogService {
public:
    Result<void> write(const LogRecord&) override { return Result<void>::success(); }
    Result<void> flush() override { return Result<void>::success(); }
};

class FailingJournalBackend final : public lasercnc::platform::IPersistenceBackend {
public:
    explicit FailingJournalBackend(
        std::unique_ptr<lasercnc::platform::IPersistenceBackend> delegate)
        : delegate_(std::move(delegate))
    {
    }

    Result<std::size_t> execute(
        std::string_view statement,
        std::span<const Value> parameters = {}) override
    {
        if(failJournal.load(std::memory_order_acquire)
           && statement.starts_with("INSERT INTO state_journal")) {
            return Result<std::size_t>::failure(makeError(
                "Test.HistoryJournalFailure",
                ErrorCategory::Infrastructure,
                "Injected history journal failure"));
        }
        return delegate_->execute(statement, parameters);
    }

    Result<std::vector<lasercnc::platform::PersistenceRow>> query(
        std::string_view statement,
        std::span<const Value> parameters = {}) override
    {
        return delegate_->query(statement, parameters);
    }

    Result<void> beginTransaction() override { return delegate_->beginTransaction(); }
    Result<void> commitTransaction() override { return delegate_->commitTransaction(); }
    Result<void> rollbackTransaction() override { return delegate_->rollbackTransaction(); }

    std::atomic_bool failJournal{false};

private:
    std::unique_ptr<lasercnc::platform::IPersistenceBackend> delegate_;
};

class CreateHandler final : public ICommandHandler {
public:
    Result<Value> execute(
        const CommandRequest& request,
        ApplicationTransaction& transaction) override
    {
        const auto* arguments = request.arguments.getIf<Value::Object>();
        const auto* idText = arguments == nullptr
            ? nullptr
            : arguments->at("id").getIf<std::string>();
        const auto* data = arguments == nullptr
            ? nullptr
            : arguments->at("data").getIf<std::string>();
        if(idText == nullptr || data == nullptr) {
            return Result<Value>::failure(makeError(
                "Test.InvalidArguments", ErrorCategory::Validation, "Invalid arguments"));
        }
        auto id = ObjectId::create(*idText);
        if(!id) {
            return Result<Value>::failure(std::move(id).error());
        }
        auto created = transaction.createObject(ObjectRecord {
            std::move(id).value(),
            validId<ObjectTypeId>("kernel.history.test"),
            Value {*data}});
        if(!created) {
            return Result<Value>::failure(std::move(created).error());
        }
        auto touched = transaction.touchRevision(RevisionScope::Geometry);
        if(!touched) {
            return Result<Value>::failure(std::move(touched).error());
        }
        return Result<Value>::success(Value {Value::Object {
            {"id", Value {*idText}},
        }});
    }
};

class ReplaceHandler final : public ICommandHandler {
public:
    Result<Value> execute(
        const CommandRequest& request,
        ApplicationTransaction& transaction) override
    {
        const auto* arguments = request.arguments.getIf<Value::Object>();
        const auto* idText = arguments == nullptr
            ? nullptr
            : arguments->at("id").getIf<std::string>();
        const auto* data = arguments == nullptr
            ? nullptr
            : arguments->at("data").getIf<std::string>();
        if(idText == nullptr || data == nullptr) {
            return Result<Value>::failure(makeError(
                "Test.InvalidArguments", ErrorCategory::Validation, "Invalid arguments"));
        }
        auto id = ObjectId::create(*idText);
        if(!id) {
            return Result<Value>::failure(std::move(id).error());
        }
        auto replaced = transaction.replaceObjectData(id.value(), Value {*data});
        if(!replaced) {
            return Result<Value>::failure(std::move(replaced).error());
        }
        return Result<Value>::success(Value {Value::Object {{"id", Value {*idText}}}});
    }
};

class RemoveHandler final : public ICommandHandler {
public:
    Result<Value> execute(
        const CommandRequest& request,
        ApplicationTransaction& transaction) override
    {
        const auto* arguments = request.arguments.getIf<Value::Object>();
        const auto* idText = arguments == nullptr
            ? nullptr
            : arguments->at("id").getIf<std::string>();
        if(idText == nullptr) {
            return Result<Value>::failure(makeError(
                "Test.InvalidArguments", ErrorCategory::Validation, "Invalid arguments"));
        }
        auto id = ObjectId::create(*idText);
        if(!id) {
            return Result<Value>::failure(std::move(id).error());
        }
        auto removed = transaction.removeObject(id.value());
        if(!removed) {
            return Result<Value>::failure(std::move(removed).error());
        }
        return Result<Value>::success(Value {Value::Object {{"id", Value {*idText}}}});
    }
};

class NoOpHandler final : public ICommandHandler {
public:
    Result<Value> execute(
        const CommandRequest&,
        ApplicationTransaction&) override
    {
        return Result<Value>::success(Value {Value::Object {}});
    }
};

CommandDescriptor createDescriptor(const char* name, bool undoable)
{
    return CommandDescriptor {
        validId<CommandName>(name),
        Version {1U, 0U, 0U},
        schema("kernel.history.test.arguments"),
        schema("kernel.history.test.result"),
        ExecutionMode::Synchronous,
        SideEffectLevel::DocumentWrite,
        validId<CapabilityId>("document.write"),
        undoable,
        true,
        true,
        ContractStatus::Active,
        ExecutionScope::Document};
}

CommandRequest request(
    const char* requestId,
    const char* command,
    const ProjectId& project,
    const DocumentId& document,
    const SessionId& session,
    const char* objectId = "unused",
    const char* data = "payload")
{
    return CommandRequest {
        validId<RequestId>(requestId),
        ExecutionContext {session, project, document},
        validId<CommandName>(command),
        Version {1U, 0U, 0U},
        Value {Value::Object {
            {"data", Value {data}},
            {"id", Value {objectId}},
        }},
        std::nullopt,
        validId<CorrelationId>("correlation.history"),
        validId<TraceId>("trace.history")};
}

std::filesystem::path databasePath()
{
    static std::atomic_ullong sequence {0U};
    return std::filesystem::temp_directory_path()
        / ("lasercnc-history-"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
           + '-' + std::to_string(sequence.fetch_add(1U)) + ".db");
}

void removeDatabase(const std::filesystem::path& path)
{
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove(path, ignored));
    static_cast<void>(std::filesystem::remove(path.string() + "-wal", ignored));
    static_cast<void>(std::filesystem::remove(path.string() + "-shm", ignored));
}

void configurePersistence(AppKernel& kernel, const std::filesystem::path& path)
{
    auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
    REQUIRE(backend.hasValue());
    REQUIRE(kernel.persistence()
                .configure(
                    std::move(backend).value(),
                    std::make_shared<JsonconsAdapter>(),
                    std::make_shared<Sha256HashService>())
                .hasValue());
}

void configureRuntime(
    AppKernel& kernel,
    const ProjectId& project,
    const DocumentId& document,
    const SessionId& session,
    bool addDocument)
{
    if(addDocument) {
        REQUIRE(kernel.addDocument(project, document).hasValue());
    }
    REQUIRE(kernel.executionServices()
                .configure(
                    std::make_shared<JsonconsAdapter>(),
                    std::make_shared<NullLog>())
                .hasValue());
    const std::array grants {
        validId<CapabilityId>("document.write"),
        validId<CapabilityId>("kernel.history.edit")};
    REQUIRE(kernel.capabilities().replace(session, grants).hasValue());
    auto handler = std::make_shared<CreateHandler>();
    REQUIRE(lasercnc::test::registerCommand(kernel,
                    createDescriptor("kernel.history.create", true), handler)
                .hasValue());
    REQUIRE(lasercnc::test::registerCommand(kernel,
                    createDescriptor("kernel.history.replace", true),
                    std::make_shared<ReplaceHandler>())
                .hasValue());
    REQUIRE(lasercnc::test::registerCommand(kernel,
                    createDescriptor("kernel.history.remove", true),
                    std::make_shared<RemoveHandler>())
                .hasValue());
    REQUIRE(lasercnc::test::registerCommand(kernel,
                    createDescriptor("kernel.history.barrier", false), handler)
                .hasValue());
    REQUIRE(lasercnc::test::registerCommand(kernel,
                    createDescriptor("kernel.history.no-op", true),
                    std::make_shared<NoOpHandler>())
                .hasValue());
    REQUIRE(lasercnc::test::registerCommand(kernel,
                    createDescriptor("kernel.history.no-op-barrier", false),
                    std::make_shared<NoOpHandler>())
                .hasValue());
}

bool hasObject(const AppKernel& kernel, const DocumentId& document, const char* id)
{
    auto snapshot = kernel.documents().snapshot(document);
    if(!snapshot) {
        return false;
    }
    return snapshot.value().objects().contains(validId<ObjectId>(id));
}

std::string objectData(const AppKernel& kernel, const DocumentId& document, const char* id)
{
    auto snapshot = kernel.documents().snapshot(document);
    if(!snapshot) {
        return {};
    }
    const auto* found = snapshot.value().objects().find(validId<ObjectId>(id));
    if(found == nullptr) {
        return {};
    }
    const auto* data = found->data.getIf<std::string>();
    return data == nullptr ? std::string {} : *data;
}

std::span<const std::byte> bytes(std::string_view value) noexcept
{
    return {
        reinterpret_cast<const std::byte*>(value.data()),
        value.size()};
}

void rewriteJournal(
    const std::filesystem::path& path,
    const char* transactionId,
    const std::function<void(Value::Object&)>& mutate)
{
    auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
    REQUIRE(backend.hasValue());
    auto rows = backend.value()->query(
        "SELECT payload FROM state_journal WHERE transaction_id=?",
        std::array<Value, 1U> {Value {transactionId}});
    REQUIRE(rows.hasValue());
    REQUIRE(rows.value().size() == 1U);
    const auto* payload = rows.value().front().at("payload").getIf<std::string>();
    REQUIRE(payload != nullptr);
    JsonconsAdapter adapter;
    auto decoded = adapter.deserialize(*payload);
    REQUIRE(decoded.hasValue());
    auto* root = decoded.value().getIf<Value::Object>();
    REQUIRE(root != nullptr);
    mutate(*root);
    auto rewritten = adapter.serialize(decoded.value());
    REQUIRE(rewritten.hasValue());
    Sha256HashService hashes;
    auto digest = hashes.digest(bytes(rewritten.value()));
    REQUIRE(digest.hasValue());
    const std::array parameters {
        Value {rewritten.value()},
        Value {std::string(digest.value().value())},
        Value {transactionId}};
    REQUIRE(backend.value()->execute(
        "UPDATE state_journal SET payload=?,digest=? WHERE transaction_id=?",
        parameters).hasValue());
}

} // namespace

TEST_CASE("HistoryRuntime executes undo redo branch replacement and barriers", "[history][runtime]")
{
    const auto project = validId<ProjectId>("project.history");
    const auto document = validId<DocumentId>("document.history");
    const auto session = validId<SessionId>("session.history");
    AppKernel kernel;
    configureRuntime(kernel, project, document, session, true);
    REQUIRE(kernel.bootstrap().hasValue());

    auto undoableNoOp = kernel.execution().executeCommand(request(
        "request.history.no-op", "kernel.history.no-op", project, document,
        session));
    REQUIRE_FALSE(undoableNoOp.hasValue());
    CHECK(std::string(undoableNoOp.error().code.value())
          == "Transaction.EmptyCommitDenied");
    auto barrierNoOp = kernel.execution().executeCommand(request(
        "request.history.no-op-barrier", "kernel.history.no-op-barrier", project,
        document, session));
    REQUIRE_FALSE(barrierNoOp.hasValue());
    CHECK(std::string(barrierNoOp.error().code.value())
          == "Transaction.EmptyCommitDenied");
    auto empty = kernel.history().snapshot(document);
    REQUIRE(empty.hasValue());
    CHECK(empty.value().cursor == HistoryCursor {});
    CHECK(empty.value().entries.empty());
    CHECK_FALSE(empty.value().barrier.has_value());

    REQUIRE(kernel.execution().executeCommand(request(
        "request.history.a", "kernel.history.create", project, document, session,
        "object.history.a")).hasValue());
    REQUIRE(kernel.execution().executeCommand(request(
        "request.history.b", "kernel.history.create", project, document, session,
        "object.history.b")).hasValue());
    auto initial = kernel.history().snapshot(document);
    REQUIRE(initial.hasValue());
    CHECK(initial.value().cursor == HistoryCursor {2U, 2U});
    CHECK_FALSE(initial.value().barrier.has_value());

    REQUIRE(kernel.execution().executeCommand(request(
        "request.history.undo-b", "edit.undo", project, document, session)).hasValue());
    CHECK_FALSE(hasObject(kernel, document, "object.history.b"));
    auto undone = kernel.history().snapshot(document);
    REQUIRE(undone.hasValue());
    CHECK(undone.value().cursor == HistoryCursor {1U, 2U});

    REQUIRE(kernel.execution().executeCommand(request(
        "request.history.redo-b", "edit.redo", project, document, session)).hasValue());
    CHECK(hasObject(kernel, document, "object.history.b"));
    REQUIRE(kernel.execution().executeCommand(request(
        "request.history.undo-b-again", "edit.undo", project, document, session)).hasValue());
    REQUIRE(kernel.execution().executeCommand(request(
        "request.history.c", "kernel.history.create", project, document, session,
        "object.history.c")).hasValue());
    auto branched = kernel.history().snapshot(document);
    REQUIRE(branched.hasValue());
    CHECK(branched.value().cursor == HistoryCursor {2U, 2U});
    CHECK(branched.value().entries[1U].transactionId
          == validId<TransactionId>("transaction.request.history.c"));
    auto noRedo = kernel.execution().executeCommand(request(
        "request.history.no-redo", "edit.redo", project, document, session));
    REQUIRE_FALSE(noRedo.hasValue());
    CHECK(std::string(noRedo.error().code.value()) == "History.RedoUnavailable");

    REQUIRE(kernel.execution().executeCommand(request(
        "request.history.barrier", "kernel.history.barrier", project, document, session,
        "object.history.barrier")).hasValue());
    auto barrier = kernel.history().snapshot(document);
    REQUIRE(barrier.hasValue());
    CHECK(barrier.value().cursor == HistoryCursor {});
    CHECK(barrier.value().entries.empty());
    REQUIRE(barrier.value().barrier.has_value());
    CHECK(barrier.value().barrier->transactionId
          == validId<TransactionId>("transaction.request.history.barrier"));
    auto blocked = kernel.execution().executeCommand(request(
        "request.history.blocked", "edit.undo", project, document, session));
    REQUIRE_FALSE(blocked.hasValue());
    CHECK(std::string(blocked.error().code.value()) == "History.UndoUnavailable");

    auto finalDocument = kernel.documents().snapshot(document);
    REQUIRE(finalDocument.hasValue());
    CHECK(finalDocument.value().revisions().at(RevisionScope::Project) == Revision {7U});
    CHECK(finalDocument.value().revisions().at(RevisionScope::Geometry) == Revision {7U});
    REQUIRE(kernel.shutdown().hasValue());
}

TEST_CASE("HistoryRuntime reverses create update and remove change shapes", "[history][changes]")
{
    const auto project = validId<ProjectId>("project.history.changes");
    const auto document = validId<DocumentId>("document.history.changes");
    const auto session = validId<SessionId>("session.history.changes");
    AppKernel kernel;
    configureRuntime(kernel, project, document, session, true);
    REQUIRE(kernel.bootstrap().hasValue());

    REQUIRE(kernel.execution().executeCommand(request(
        "request.history.changes-create", "kernel.history.create", project, document,
        session, "object.history.changes", "original")).hasValue());
    REQUIRE(kernel.execution().executeCommand(request(
        "request.history.changes-update", "kernel.history.replace", project, document,
        session, "object.history.changes", "updated")).hasValue());
    CHECK(objectData(kernel, document, "object.history.changes") == "updated");

    REQUIRE(kernel.execution().executeCommand(request(
        "request.history.changes-undo-update", "edit.undo", project, document, session))
                .hasValue());
    CHECK(objectData(kernel, document, "object.history.changes") == "original");
    REQUIRE(kernel.execution().executeCommand(request(
        "request.history.changes-redo-update", "edit.redo", project, document, session))
                .hasValue());
    CHECK(objectData(kernel, document, "object.history.changes") == "updated");

    REQUIRE(kernel.execution().executeCommand(request(
        "request.history.changes-remove", "kernel.history.remove", project, document,
        session, "object.history.changes")).hasValue());
    CHECK_FALSE(hasObject(kernel, document, "object.history.changes"));
    REQUIRE(kernel.execution().executeCommand(request(
        "request.history.changes-undo-remove", "edit.undo", project, document, session))
                .hasValue());
    CHECK(objectData(kernel, document, "object.history.changes") == "updated");
    REQUIRE(kernel.execution().executeCommand(request(
        "request.history.changes-redo-remove", "edit.redo", project, document, session))
                .hasValue());
    CHECK_FALSE(hasObject(kernel, document, "object.history.changes"));

    auto history = kernel.history().snapshot(document);
    REQUIRE(history.hasValue());
    CHECK(history.value().cursor == HistoryCursor {3U, 3U});
    REQUIRE(kernel.shutdown().hasValue());
}

TEST_CASE("History cursor and redo material survive a normal restart", "[history][persistence]")
{
    const auto path = databasePath();
    removeDatabase(path);
    const auto project = validId<ProjectId>("project.history.restart");
    const auto document = validId<DocumentId>("document.history.restart");
    const auto session = validId<SessionId>("session.history.restart");
    {
        AppKernel kernel;
        configurePersistence(kernel, path);
        configureRuntime(kernel, project, document, session, true);
        REQUIRE(kernel.bootstrap().hasValue());
        auto first = request(
            "request.history.restart-a", "kernel.history.create", project, document,
            session, "object.history.restart-a");
        first.idempotencyKey = validId<IdempotencyKey>("idempotency.history.restart-a");
        REQUIRE(kernel.execution().executeCommand(first).hasValue());
        REQUIRE(kernel.execution().executeCommand(request(
            "request.history.restart-b", "kernel.history.create", project, document,
            session, "object.history.restart-b")).hasValue());
        REQUIRE(kernel.execution().executeCommand(request(
            "request.history.restart-undo", "edit.undo", project, document, session))
                    .hasValue());
        REQUIRE(kernel.shutdown().hasValue());
    }
    {
        AppKernel kernel;
        configurePersistence(kernel, path);
        configureRuntime(kernel, project, document, session, false);
        REQUIRE(kernel.bootstrap().hasValue());
        auto restored = kernel.history().snapshot(document);
        REQUIRE(restored.hasValue());
        CHECK(restored.value().cursor == HistoryCursor {1U, 2U});
        CHECK(hasObject(kernel, document, "object.history.restart-a"));
        CHECK_FALSE(hasObject(kernel, document, "object.history.restart-b"));
        auto replayRequest = request(
            "request.history.restart-a", "kernel.history.create", project, document,
            session, "object.history.restart-a");
        replayRequest.idempotencyKey = validId<IdempotencyKey>(
            "idempotency.history.restart-a");
        auto replay = kernel.execution().executeCommand(replayRequest);
        REQUIRE(replay.hasValue());
        CHECK(replay.value().replayed);
        REQUIRE(replay.value().commit.has_value());
        CHECK(replay.value().commit->history.kind == HistoryMutationKind::Record);
        auto afterReplay = kernel.history().snapshot(document);
        REQUIRE(afterReplay.hasValue());
        CHECK(afterReplay.value().cursor == HistoryCursor {1U, 2U});
        REQUIRE(kernel.execution().executeCommand(request(
            "request.history.restart-redo", "edit.redo", project, document, session))
                    .hasValue());
        CHECK(hasObject(kernel, document, "object.history.restart-b"));
        REQUIRE(kernel.shutdown().hasValue());
    }
    removeDatabase(path);
}

TEST_CASE("Journal failure changes neither document nor history", "[history][persistence][failure]")
{
    const auto path = databasePath();
    removeDatabase(path);
    const auto project = validId<ProjectId>("project.history.failure");
    const auto document = validId<DocumentId>("document.history.failure");
    const auto session = validId<SessionId>("session.history.failure");
    {
        AppKernel kernel;
        auto sqlite = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(sqlite.hasValue());
        auto backend = std::make_unique<FailingJournalBackend>(
            std::move(sqlite).value());
        auto* control = backend.get();
        REQUIRE(kernel.persistence().configure(
            std::move(backend),
            std::make_shared<JsonconsAdapter>(),
            std::make_shared<Sha256HashService>()).hasValue());
        configureRuntime(kernel, project, document, session, true);
        REQUIRE(kernel.bootstrap().hasValue());
        control->failJournal.store(true, std::memory_order_release);

        auto failed = kernel.execution().executeCommand(request(
            "request.history.failure", "kernel.history.create", project, document,
            session, "object.history.failure"));
        REQUIRE_FALSE(failed.hasValue());
        CHECK_FALSE(hasObject(kernel, document, "object.history.failure"));
        auto state = kernel.documents().snapshot(document);
        REQUIRE(state.hasValue());
        CHECK(state.value().revisions().at(RevisionScope::Project) == Revision {});
        auto history = kernel.history().snapshot(document);
        REQUIRE(history.hasValue());
        CHECK(history.value().cursor == HistoryCursor {});
        CHECK(history.value().entries.empty());
        REQUIRE(kernel.shutdown().hasValue());
    }
    removeDatabase(path);
}

TEST_CASE("History recovery fails closed on semantically invalid journal metadata", "[history][persistence][tamper]")
{
    const auto path = databasePath();
    removeDatabase(path);
    const auto project = validId<ProjectId>("project.history.tamper");
    const auto document = validId<DocumentId>("document.history.tamper");
    const auto session = validId<SessionId>("session.history.tamper");
    {
        AppKernel kernel;
        configurePersistence(kernel, path);
        configureRuntime(kernel, project, document, session, true);
        REQUIRE(kernel.bootstrap().hasValue());
        REQUIRE(kernel.execution().executeCommand(request(
            "request.history.tamper", "kernel.history.create", project, document,
            session, "object.history.tamper")).hasValue());
        REQUIRE(kernel.shutdown().hasValue());
    }
    rewriteJournal(
        path,
        "transaction.request.history.tamper",
        [](Value::Object& root) {
        auto* history = root.at("history").getIf<Value::Object>();
        REQUIRE(history != nullptr);
        history->insert_or_assign("kind", Value {"undo"});
        });
    {
        AppKernel kernel;
        configurePersistence(kernel, path);
        configureRuntime(kernel, project, document, session, false);
        auto started = kernel.bootstrap();
        REQUIRE_FALSE(started.hasValue());
        CHECK(std::string(started.error().code.value())
              == "Persistence.KernelRecoveryFailed");
    }
    removeDatabase(path);
}

TEST_CASE("History recovery rejects unknown fields with a valid digest", "[history][persistence][tamper]")
{
    const auto path = databasePath();
    removeDatabase(path);
    const auto project = validId<ProjectId>("project.history.unknown");
    const auto document = validId<DocumentId>("document.history.unknown");
    const auto session = validId<SessionId>("session.history.unknown");
    {
        AppKernel kernel;
        configurePersistence(kernel, path);
        configureRuntime(kernel, project, document, session, true);
        REQUIRE(kernel.bootstrap().hasValue());
        REQUIRE(kernel.execution().executeCommand(request(
            "request.history.unknown", "kernel.history.create", project, document,
            session, "object.history.unknown")).hasValue());
        REQUIRE(kernel.shutdown().hasValue());
    }
    rewriteJournal(
        path,
        "transaction.request.history.unknown",
        [](Value::Object& root) {
            auto* history = root.at("history").getIf<Value::Object>();
            REQUIRE(history != nullptr);
            history->insert_or_assign("unexpected", Value {true});
        });
    {
        AppKernel kernel;
        configurePersistence(kernel, path);
        configureRuntime(kernel, project, document, session, false);
        auto started = kernel.bootstrap();
        REQUIRE_FALSE(started.hasValue());
        CHECK(std::string(started.error().code.value())
              == "Persistence.KernelRecoveryFailed");
    }
    removeDatabase(path);
}

TEST_CASE("Version one journal writes become an undo barrier", "[history][persistence][compatibility]")
{
    const auto path = databasePath();
    removeDatabase(path);
    const auto project = validId<ProjectId>("project.history.v1");
    const auto document = validId<DocumentId>("document.history.v1");
    const auto session = validId<SessionId>("session.history.v1");
    {
        AppKernel kernel;
        configurePersistence(kernel, path);
        configureRuntime(kernel, project, document, session, true);
        REQUIRE(kernel.bootstrap().hasValue());
        REQUIRE(kernel.execution().executeCommand(request(
            "request.history.v1", "kernel.history.create", project, document,
            session, "object.history.v1")).hasValue());
        REQUIRE(kernel.shutdown().hasValue());
    }
    rewriteJournal(
        path,
        "transaction.request.history.v1",
        [](Value::Object& root) {
            root.insert_or_assign("version", Value {std::int64_t {1}});
            root.erase("history");
        });
    {
        AppKernel kernel;
        configurePersistence(kernel, path);
        configureRuntime(kernel, project, document, session, false);
        REQUIRE(kernel.bootstrap().hasValue());
        auto history = kernel.history().snapshot(document);
        REQUIRE(history.hasValue());
        CHECK(history.value().entries.empty());
        CHECK(history.value().cursor == HistoryCursor {});
        REQUIRE(history.value().barrier.has_value());
        CHECK(hasObject(kernel, document, "object.history.v1"));
        REQUIRE(kernel.shutdown().hasValue());
    }
    removeDatabase(path);
}
