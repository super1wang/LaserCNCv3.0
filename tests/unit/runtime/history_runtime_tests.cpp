#include <lasercnc/infrastructure/jsoncons_adapter.hpp>
#include <lasercnc/infrastructure/filesystem_snapshot_store.hpp>
#include <lasercnc/infrastructure/sha256_hash_service.hpp>
#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>
#include <lasercnc/kernel/app_kernel.hpp>
#include <lasercnc/observability/log_service.hpp>

#include <catch2/catch_test_macros.hpp>
#include "kernel_test_module.hpp"
#include "persistence_fixture.hpp"
#include "fault_injecting_backend.hpp"
#include "fault_injecting_data_plane.hpp"

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
    Result<lasercnc::platform::PersistenceSessionInfo> acquireHostSession() override
    { return delegate_->acquireHostSession(); }
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
    REQUIRE(kernel.configurePersistence(
                    std::move(backend).value(),
                    std::make_shared<JsonconsAdapter>(),
                    std::make_shared<Sha256HashService>())
                .hasValue());
}

class HistoryObjectMigration final : public IObjectMigration {
public:
    Result<Value> migrate(const Value& data) const override
    {
        const auto* text = data.getIf<std::string>();
        if(text == nullptr) {
            return Result<Value>::failure(makeError(
                "Test.InvalidMigrationData", ErrorCategory::Validation, "Expected string data"));
        }
        return Result<Value>::success(Value {*text + ".v2"});
    }
};

class MigrateHandler final : public ICommandHandler {
public:
    Result<Value> execute(const CommandRequest& request, ApplicationTransaction& transaction) override
    {
        const auto& arguments = *request.arguments.getIf<Value::Object>();
        auto object = ObjectId::create(*arguments.at("id").getIf<std::string>());
        if(!object) {
            return Result<Value>::failure(std::move(object).error());
        }
        auto migrated = transaction.migrateObject(object.value(), Version {2U, 0U, 0U});
        if(!migrated) {
            return Result<Value>::failure(std::move(migrated).error());
        }
        return Result<Value>::success(Value {Value::Object{}});
    }
};

void configureRuntime(
    AppKernel& kernel,
    const ProjectId& project,
    const DocumentId& document,
    const SessionId& session,
    bool addDocument)
{
    auto objectType = lasercnc::test::valueObjectType("kernel.history.test");
    objectType.descriptor.currentVersion = Version {2U, 0U, 0U};
    objectType.versions.push_back({Version {2U, 0U, 0U},
        std::make_shared<lasercnc::test::TestValueObjectValidator>(),
        std::make_shared<lasercnc::test::TestEmptyObjectReferences>()});
    objectType.migrations.push_back({Version {1U, 0U, 0U}, Version {2U, 0U, 0U},
        std::make_shared<HistoryObjectMigration>()});
    REQUIRE(lasercnc::test::registerObjectType(kernel, std::move(objectType)).hasValue());
    REQUIRE(lasercnc::test::registerCommand(kernel,
        createDescriptor("kernel.history.migrate", true), std::make_shared<MigrateHandler>()).hasValue());
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

namespace {
std::filesystem::path projectRevisionRoot()
{
    static std::atomic_uint sequence{0U};
    const auto tick = std::chrono::system_clock::now().time_since_epoch().count();
    auto root = std::filesystem::path{LCNC_STRESS_TEST_ROOT} / "project-revision"
        / (std::to_string(tick) + "-" + std::to_string(sequence.fetch_add(1U)));
    std::filesystem::create_directories(root);
    return root;
}

void configureProjectRevisionPersistence(AppKernel& kernel, const std::filesystem::path& root)
{
    auto backend = SqlitePersistenceBackend::open({root / "state.db"});
    REQUIRE(backend.hasValue());
    auto snapshots = FilesystemSnapshotStore::create({root / "snapshots", 1024U * 1024U});
    REQUIRE(snapshots.hasValue());
    REQUIRE(kernel.configurePersistence(std::move(backend).value(), std::make_shared<JsonconsAdapter>(),
        std::make_shared<Sha256HashService>(), std::move(snapshots).value()).hasValue());
}

void checkProjectDocumentRevision(const AppKernel& kernel, const DocumentId& document,
    std::uint64_t projectRevision, std::uint64_t documentRevision)
{
    auto snapshot = kernel.documents().snapshot(document);
    REQUIRE(snapshot.hasValue());
    CHECK(snapshot.value().revisions().at(RevisionScope::Project).value() == projectRevision);
    CHECK(snapshot.value().revisions().at(RevisionScope::Document).value() == documentRevision);
}
}

TEST_CASE("Project revision survives restart with no loaded children and a subsequent commit", "[project-revision][recovery]")
{
    bool closeProject = false;
    bool removeDocuments = false;
    bool configuredNewDocument = false;
    SECTION("all children Detached") {}
    SECTION("closed project reopened without its old children") { closeProject = true; }
    SECTION("all children Removed") { removeDocuments = true; }
    SECTION("new child configured before recovery") { configuredNewDocument = true; }
    const auto root = projectRevisionRoot();
    INFO("Retained recovery fixture: " << root.string());
    const auto project = validId<ProjectId>("project.revision");
    const auto a = validId<DocumentId>("document.revision.a");
    const auto b = validId<DocumentId>("document.revision.b");
    const auto c = validId<DocumentId>("document.revision.c");
    const auto session = validId<SessionId>("session.revision");
    {
        AppKernel kernel;
        configureProjectRevisionPersistence(kernel, root);
        configureRuntime(kernel, project, a, session, true);
        REQUIRE(kernel.addDocument(project, b));
        REQUIRE(kernel.bootstrap());
        REQUIRE(kernel.execution().executeCommand(request("request.revision.a", "kernel.history.create",
            project, a, session, "object.a")));
        REQUIRE(kernel.execution().executeCommand(request("request.revision.b", "kernel.history.create",
            project, b, session, "object.b")));
        checkProjectDocumentRevision(kernel, a, 2U, 1U);
        if(closeProject) {
            REQUIRE(kernel.projectRuntime().close(project));
        } else {
            REQUIRE(kernel.documentRuntime().close(a));
            REQUIRE(kernel.documentRuntime().close(b));
        }
        if(removeDocuments) {
            REQUIRE(kernel.documentRuntime().remove(a));
            REQUIRE(kernel.documentRuntime().remove(b));
        }
        REQUIRE(kernel.shutdown());
    }
    {
        AppKernel kernel;
        configureProjectRevisionPersistence(kernel, root);
        configureRuntime(kernel, project, c, session, configuredNewDocument);
        REQUIRE(kernel.bootstrap());
        CHECK_FALSE(kernel.documents().contains(a));
        CHECK_FALSE(kernel.documents().contains(b));
        if(closeProject) {
            CHECK(kernel.projectRuntime().lifecycle(project).value().state == ProjectLifecycleState::Closed);
            REQUIRE(kernel.projectRuntime().open(project));
        }
        if(!configuredNewDocument) { REQUIRE(kernel.documentRuntime().create(project, c)); }
        checkProjectDocumentRevision(kernel, c, 2U, 0U);
        REQUIRE(kernel.execution().executeCommand(request("request.revision.c", "kernel.history.create",
            project, c, session, "object.c")));
        checkProjectDocumentRevision(kernel, c, 3U, 1U);
        REQUIRE(kernel.shutdown());
    }
    {
        AppKernel kernel;
        configureProjectRevisionPersistence(kernel, root);
        configureRuntime(kernel, project, c, session, false);
        auto booted = kernel.bootstrap();
        INFO("Recovery result: " << (booted ? std::string_view{"success"} : booted.error().code.value()));
        REQUIRE(booted);
        checkProjectDocumentRevision(kernel, c, 3U, 1U);
        CHECK(hasObject(kernel, c, "object.c"));
        CHECK(kernel.history().snapshot(c).value().cursor == HistoryCursor{1U, 1U});
        CHECK_FALSE(kernel.documents().contains(a));
        CHECK_FALSE(kernel.documents().contains(b));
        if(removeDocuments) {
            CHECK_FALSE(kernel.documentRuntime().open(a));
        } else {
            REQUIRE(kernel.documentRuntime().open(a));
            checkProjectDocumentRevision(kernel, a, 3U, 1U);
            CHECK(hasObject(kernel, a, "object.a"));
            CHECK(kernel.history().snapshot(a).value().cursor == HistoryCursor{1U, 1U});
        }
        REQUIRE(kernel.shutdown());
    }
}

TEST_CASE("Project revision recovery isolates projects with mixed open and detached children", "[project-revision][recovery]")
{
    const auto root = projectRevisionRoot();
    INFO("Retained recovery fixture: " << root.string());
    const auto p = validId<ProjectId>("project.p");
    const auto q = validId<ProjectId>("project.q");
    const auto a = validId<DocumentId>("document.p.a");
    const auto b = validId<DocumentId>("document.p.b");
    const auto c = validId<DocumentId>("document.p.c");
    const auto d = validId<DocumentId>("document.q.d");
    const auto e = validId<DocumentId>("document.q.e");
    const auto session = validId<SessionId>("session.revision");
    {
        AppKernel kernel;
        configureProjectRevisionPersistence(kernel, root);
        configureRuntime(kernel, p, a, session, true);
        REQUIRE(kernel.addDocument(p, b));
        REQUIRE(kernel.addDocument(q, d));
        REQUIRE(kernel.bootstrap());
        REQUIRE(kernel.execution().executeCommand(request("request.a", "kernel.history.create", p, a, session, "object.a")));
        REQUIRE(kernel.execution().executeCommand(request("request.b", "kernel.history.create", p, b, session, "object.b")));
        REQUIRE(kernel.execution().executeCommand(request("request.d", "kernel.history.create", q, d, session, "object.d")));
        REQUIRE(kernel.documentRuntime().close(b));
        REQUIRE(kernel.documentRuntime().close(d));
        REQUIRE(kernel.shutdown());
    }
    {
        AppKernel kernel;
        configureProjectRevisionPersistence(kernel, root);
        configureRuntime(kernel, p, a, session, false);
        REQUIRE(kernel.bootstrap());
        checkProjectDocumentRevision(kernel, a, 2U, 1U);
        CHECK_FALSE(kernel.documents().contains(b));
        CHECK_FALSE(kernel.documents().contains(d));
        REQUIRE(kernel.documentRuntime().create(p, c));
        REQUIRE(kernel.documentRuntime().create(q, e));
        checkProjectDocumentRevision(kernel, c, 2U, 0U);
        checkProjectDocumentRevision(kernel, e, 1U, 0U);
        REQUIRE(kernel.execution().executeCommand(request("request.c", "kernel.history.create", p, c, session, "object.c")));
        REQUIRE(kernel.execution().executeCommand(request("request.e", "kernel.history.create", q, e, session, "object.e")));
        REQUIRE(kernel.documentRuntime().open(b));
        REQUIRE(kernel.documentRuntime().open(d));
        checkProjectDocumentRevision(kernel, b, 3U, 1U);
        checkProjectDocumentRevision(kernel, d, 2U, 1U);
        REQUIRE(kernel.shutdown());
    }
    {
        AppKernel kernel;
        configureProjectRevisionPersistence(kernel, root);
        configureRuntime(kernel, p, a, session, false);
        REQUIRE(kernel.bootstrap());
        for(const auto& document : {a, b, c}) { checkProjectDocumentRevision(kernel, document, 3U, 1U); }
        for(const auto& document : {d, e}) { checkProjectDocumentRevision(kernel, document, 2U, 1U); }
        CHECK(hasObject(kernel, b, "object.b"));
        CHECK(hasObject(kernel, d, "object.d"));
        CHECK(kernel.history().snapshot(c).value().cursor == HistoryCursor{1U, 1U});
        CHECK(kernel.history().snapshot(e).value().cursor == HistoryCursor{1U, 1U});
        REQUIRE(kernel.shutdown());
    }
}

TEST_CASE("Recovery object admission runs before module initialization and rolls back contributions", "[persistence][object-type][admission]")
{
    class ObserverModule final : public IModule {
    public:
        ObserverModule(int& initialized, int& started) : initialized_(initialized), started_(started) {}
        const ModuleDescriptor& descriptor() const noexcept override { return descriptor_; }
        Result<void> registerComponents(ModuleRegistrar&) override { return Result<void>::success(); }
        Result<void> initialize(AppKernel&) override { ++initialized_; return Result<void>::success(); }
        Result<void> start(AppKernel&) override { ++started_; return Result<void>::success(); }
    private:
        ModuleDescriptor descriptor_ {validId<ModuleId>("module.admission.observer"), "observer", Version {1U, 0U, 0U}};
        int& initialized_;
        int& started_;
    };
    const auto path = databasePath();
    const auto project = validId<ProjectId>("project.history");
    const auto document = validId<DocumentId>("document.history");
    const auto session = validId<SessionId>("session.history");
    bool removeObject = false;
    bool invalidVersion = false;
    bool dangling = false;
    SECTION("unknown current type") {}
    SECTION("unsupported exact schema") { invalidVersion = true; }
    SECTION("unknown type in history after object removal") { removeObject = true; }
    SECTION("dangling reference in recovered state") { dangling = true; }
    {
        AppKernel kernel;
        configurePersistence(kernel, path);
        configureRuntime(kernel, project, document, session, true);
        REQUIRE(kernel.bootstrap().hasValue());
        REQUIRE(kernel.execution().executeCommand(request(
            "request.admission.seed", "kernel.history.create", project, document, session,
            "object.admission")).hasValue());
        if(removeObject) {
            REQUIRE(kernel.execution().executeCommand(request(
                "request.admission.remove", "kernel.history.remove", project, document, session,
                "object.admission")).hasValue());
        }
        REQUIRE(kernel.shutdown().hasValue());
    }
    {
        AppKernel kernel;
        configurePersistence(kernel, path);
        int initialized = 0;
        int started = 0;
        REQUIRE(kernel.addModule(std::make_unique<ObserverModule>(initialized, started)).hasValue());
        auto type = lasercnc::test::valueObjectType(
            (invalidVersion || dangling) ? "kernel.history.test" : "type.unrelated",
            invalidVersion ? Version {2U, 0U, 0U} : Version {1U, 0U, 0U});
        if(dangling) {
            class DanglingReferences final : public IObjectReferenceEnumerator {
            public:
                Result<std::vector<ObjectId>> enumerate(const Value&) const override
                {
                    return Result<std::vector<ObjectId>>::success({validId<ObjectId>("object.missing")});
                }
            };
            type.versions.front().references = std::make_shared<DanglingReferences>();
        }
        REQUIRE(lasercnc::test::registerObjectType(kernel, std::move(type)).hasValue());
        auto bootstrapped = kernel.bootstrap();
        REQUIRE_FALSE(bootstrapped.hasValue());
        CHECK(std::string(bootstrapped.error().code.value()) ==
            (removeObject ? "ObjectType.HistoryAdmissionFailed" : "ObjectType.RecoveryAdmissionFailed"));
        CHECK(initialized == 0);
        CHECK(started == 0);
        CHECK(kernel.objectTypes().size() == 0U);
        CHECK_FALSE(kernel.documents().contains(document));
        CHECK_FALSE(kernel.documentRuntime().accepting());
        CHECK(kernel.state() == AppKernelState::Failed);
    }
    removeDatabase(path);
}

TEST_CASE("Recovery refuses stripped or malformed object versions despite a valid digest", "[persistence][object-type][recovery]")
{
    const auto path = databasePath();
    const auto project = validId<ProjectId>("project.history");
    const auto document = validId<DocumentId>("document.history");
    const auto session = validId<SessionId>("session.history");
    {
        AppKernel kernel;
        configurePersistence(kernel, path);
        configureRuntime(kernel, project, document, session, true);
        REQUIRE(kernel.bootstrap().hasValue());
        REQUIRE(kernel.execution().executeCommand(request(
            "request.schema.invalid", "kernel.history.create", project, document, session,
            "object.history.versioned")).hasValue());
        REQUIRE(kernel.shutdown().hasValue());
    }
    SECTION("new journal cannot omit schemaVersion") {
        rewriteJournal(path, "transaction.request.schema.invalid", [](Value::Object& root) {
            auto& change = root.at("changes").getIf<Value::Array>()->front();
            change.getIf<Value::Object>()->at("after").getIf<Value::Object>()->erase("schemaVersion");
        });
    }
    SECTION("new journal rejects overflow version") {
        rewriteJournal(path, "transaction.request.schema.invalid", [](Value::Object& root) {
            auto& change = root.at("changes").getIf<Value::Array>()->front();
            auto& object = *change.getIf<Value::Object>()->at("after").getIf<Value::Object>();
            object.at("schemaVersion").getIf<Value::Object>()->at("major") = Value {std::int64_t {4294967296LL}};
        });
    }
    SECTION("legacy version cannot silently reinterpret a new object envelope") {
        rewriteJournal(path, "transaction.request.schema.invalid", [](Value::Object& root) {
            root.at("version") = Value {std::int64_t {2}};
        });
    }
    {
        AppKernel kernel;
        configurePersistence(kernel, path);
        configureRuntime(kernel, project, document, session, false);
        CHECK_FALSE(kernel.bootstrap().hasValue());
        CHECK_FALSE(kernel.documentRuntime().accepting());
        CHECK(kernel.state() == AppKernelState::Failed);
    }
    removeDatabase(path);
}

TEST_CASE("History restores exact object versions after migration and restart", "[history][object-type][recovery]")
{
    const auto path = databasePath();
    const auto project = validId<ProjectId>("project.history");
    const auto document = validId<DocumentId>("document.history");
    const auto session = validId<SessionId>("session.history");
    const auto object = validId<ObjectId>("object.history.versioned");
    {
        AppKernel kernel;
        configurePersistence(kernel, path);
        configureRuntime(kernel, project, document, session, true);
        REQUIRE(kernel.bootstrap().hasValue());
        REQUIRE(kernel.execution().executeCommand(request(
            "request.schema.create", "kernel.history.create", project, document, session,
            "object.history.versioned", "original")).hasValue());
        REQUIRE(kernel.execution().executeCommand(request(
            "request.schema.migrate", "kernel.history.migrate", project, document, session,
            "object.history.versioned")).hasValue());
        CHECK(kernel.documents().snapshot(document).value().objects().find(object)->schemaVersion == Version {2U, 0U, 0U});
        CHECK(objectData(kernel, document, "object.history.versioned") == "original.v2");
        REQUIRE(kernel.shutdown().hasValue());
    }
    {
        AppKernel kernel;
        configurePersistence(kernel, path);
        configureRuntime(kernel, project, document, session, false);
        REQUIRE(kernel.bootstrap().hasValue());
        REQUIRE(kernel.execution().executeCommand(request(
            "request.schema.undo", "edit.undo", project, document, session)).hasValue());
        CHECK(kernel.documents().snapshot(document).value().objects().find(object)->schemaVersion == Version {1U, 0U, 0U});
        CHECK(objectData(kernel, document, "object.history.versioned") == "original");
        REQUIRE(kernel.shutdown().hasValue());
    }
    {
        AppKernel kernel;
        configurePersistence(kernel, path);
        configureRuntime(kernel, project, document, session, false);
        REQUIRE(kernel.bootstrap().hasValue());
        REQUIRE(kernel.execution().executeCommand(request(
            "request.schema.redo", "edit.redo", project, document, session)).hasValue());
        CHECK(kernel.documents().snapshot(document).value().objects().find(object)->schemaVersion == Version {2U, 0U, 0U});
        CHECK(objectData(kernel, document, "object.history.versioned") == "original.v2");
        REQUIRE(kernel.shutdown().hasValue());
    }
    removeDatabase(path);
}

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
        REQUIRE(kernel.configurePersistence(
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

TEST_CASE("Persistence stage failures preserve history revisions and idempotency across restart", "[history][persistence][fault-matrix]")
{
    using lasercnc::test::BackendPoint;
    struct Stage { const char* name; BackendPoint point; const char* sql; unsigned int occurrence; };
    const std::array stages{
        Stage{"claim begin", BackendPoint::Begin, "", 1U},
        Stage{"journal begin", BackendPoint::Begin, "", 2U},
        Stage{"journal insert", BackendPoint::Execute, "INSERT INTO state_journal", 1U},
        Stage{"journal readback", BackendPoint::Query, "FROM state_journal WHERE transaction_id=", 2U},
        Stage{"idempotency completion", BackendPoint::Execute, "UPDATE command_idempotency SET status='completed'", 1U},
        Stage{"journal commit", BackendPoint::Commit, "", 2U},
    };
    for(const auto& stage : stages) {
        for(const bool throws : {false, true}) {
            for(const std::string action : {"create", "undo", "redo"}) {
                if(action != "create" && (std::string(stage.name) == "claim begin"
                    || std::string(stage.name) == "idempotency completion")) {
                    continue;
                }
                DYNAMIC_SECTION(stage.name << " throws=" << throws << " action=" << action) {
                    const auto path = databasePath();
                    const auto project = validId<ProjectId>("project.matrix");
                    const auto document = validId<DocumentId>("document.matrix");
                    const auto session = validId<SessionId>("session.matrix");
                    auto attempted = request("request.matrix.attempt", "kernel.history.create", project,
                        document, session, "object.matrix.new");
                    if(action != "create") {
                        attempted.command = validId<CommandName>(action == "undo" ? "edit.undo" : "edit.redo");
                        attempted.arguments = Value{Value::Object{}};
                    }
                    if(action == "create") {
                        attempted.idempotencyKey = validId<IdempotencyKey>("key.matrix");
                    }
                    RevisionSet expectedRevisions;
                    HistoryCursor expectedCursor;
                    std::vector<ObjectRecord> expectedObjects;
                    std::size_t expectedJournal = 0U;
                    {
                        AppKernel kernel;
                        auto sqlite = SqlitePersistenceBackend::open({path});
                        REQUIRE(sqlite.hasValue());
                        auto backend = std::make_unique<lasercnc::test::FaultInjectingBackend>(std::move(sqlite).value());
                        auto* control = backend.get();
                        REQUIRE(kernel.configurePersistence(std::move(backend),
                            std::make_shared<JsonconsAdapter>(), std::make_shared<Sha256HashService>()).hasValue());
                        configureRuntime(kernel, project, document, session, true);
                        REQUIRE(kernel.bootstrap().hasValue());
                        REQUIRE(kernel.execution().executeCommand(request("request.matrix.seed", "kernel.history.create",
                            project, document, session, "object.matrix.seed")).hasValue());
                        if(action == "redo") {
                            REQUIRE(kernel.execution().executeCommand(request("request.matrix.seed-undo", "edit.undo",
                                project, document, session)).hasValue());
                        }
                        expectedRevisions = kernel.documents().snapshot(document).value().revisions();
                        expectedObjects = kernel.documents().snapshot(document).value().objects().all();
                        expectedCursor = kernel.history().snapshot(document).value().cursor;
                        expectedJournal = kernel.persistence().journalAfter(document, 0U).value().size();
                        const auto occurrence = action != "create"
                            && (stage.point == BackendPoint::Begin || stage.point == BackendPoint::Commit)
                            ? 1U : stage.occurrence;
                        control->arm(stage.point, stage.sql, occurrence, throws);
                        auto rejected = kernel.execution().executeCommand(attempted);
                        REQUIRE_FALSE(rejected.hasValue());
                        INFO(rejected.error().message);
                        CHECK(control->hits == 1U);
                        CHECK(kernel.documents().snapshot(document).value().objects().all() == expectedObjects);
                        CHECK(kernel.documents().snapshot(document).value().revisions() == expectedRevisions);
                        CHECK(kernel.history().snapshot(document).value().cursor == expectedCursor);
                        CHECK(kernel.persistence().journalAfter(document, 0U).value().size() == expectedJournal);
                        auto pending = control->query("SELECT * FROM command_idempotency");
                        REQUIRE(pending.hasValue());
                        CHECK(pending.value().empty());
                        REQUIRE(kernel.shutdown().hasValue());
                    }
                    {
                        AppKernel kernel;
                        configurePersistence(kernel, path);
                        configureRuntime(kernel, project, document, session, false);
                        REQUIRE(kernel.bootstrap().hasValue());
                        CHECK(kernel.documents().snapshot(document).value().objects().all() == expectedObjects);
                        CHECK(kernel.documents().snapshot(document).value().revisions() == expectedRevisions);
                        CHECK(kernel.history().snapshot(document).value().cursor == expectedCursor);
                        auto retried = kernel.execution().executeCommand(attempted);
                        REQUIRE(retried.hasValue());
                        CHECK_FALSE(retried.value().replayed);
                        CHECK(kernel.persistence().journalAfter(document, 0U).value().size() == expectedJournal + 1U);
                        const auto cursor = kernel.history().snapshot(document).value().cursor;
                        CHECK(cursor.position == (action == "undo" ? expectedCursor.position - 1U : expectedCursor.position + 1U));
                        if(action == "create") {
                            auto replayed = kernel.execution().executeCommand(attempted);
                            REQUIRE(replayed.hasValue());
                            CHECK(replayed.value().replayed);
                            CHECK(kernel.history().snapshot(document).value().cursor == cursor);
                        }
                        CHECK(kernel.persistence().journalAfter(document, 0U).value().size() == expectedJournal + 1U);
                        REQUIRE(kernel.shutdown().hasValue());
                    }
                    removeDatabase(path);
                }
            }
        }
    }
}

TEST_CASE("Rollback failure quarantines persistence until a fresh recovery", "[history][persistence][fault-matrix]")
{
    for(const bool throws : {false, true}) {
        DYNAMIC_SECTION("rollback throws=" << throws) {
            const auto path = databasePath();
            const auto project = validId<ProjectId>("project.rollback-fault");
            const auto document = validId<DocumentId>("document.rollback-fault");
            const auto session = validId<SessionId>("session.rollback-fault");
            const auto command = request("request.rollback-fault", "kernel.history.create",
                project, document, session, "object.rollback-fault");
            {
                AppKernel kernel;
                auto sqlite = SqlitePersistenceBackend::open({path});
                REQUIRE(sqlite.hasValue());
                auto backend = std::make_unique<lasercnc::test::FaultInjectingBackend>(std::move(sqlite).value());
                auto* control = backend.get();
                REQUIRE(kernel.configurePersistence(std::move(backend), std::make_shared<JsonconsAdapter>(),
                    std::make_shared<Sha256HashService>()).hasValue());
                configureRuntime(kernel, project, document, session, true);
                REQUIRE(kernel.bootstrap().hasValue());
                control->arm(lasercnc::test::BackendPoint::Commit, "", 1U, false);
                control->failRollback = true;
                control->throwRollback = throws;
                auto rejected = kernel.execution().executeCommand(command);
                REQUIRE_FALSE(rejected.hasValue());
                CHECK(std::string(rejected.error().code.value()) == "Persistence.RollbackFailed");
                CHECK(control->hits == 1U);
                CHECK(control->rollbackHits == 1U);
                CHECK(kernel.documents().snapshot(document).value().objects().empty());
                CHECK(kernel.documents().snapshot(document).value().revisions() == RevisionSet{});
                CHECK(kernel.history().snapshot(document).value().cursor == HistoryCursor{});
                CHECK_FALSE(kernel.persistence().ready());
                CHECK_FALSE(kernel.persistence().journalAfter(document, 0U).hasValue());
                auto reconfigured = kernel.configurePersistence(nullptr, nullptr, nullptr);
                REQUIRE_FALSE(reconfigured.hasValue());
                CHECK(std::string(reconfigured.error().code.value()) == "Kernel.PersistenceConfigurationClosed");
                CHECK_FALSE(kernel.execution().executeCommand(command).hasValue());
                // An independent connection must never observe the failed transaction.
                // 中文翻译：独立连接不得观察到失败事务中的记录。
                auto observer = SqlitePersistenceBackend::open({path});
                REQUIRE(observer.hasValue());
                auto rows = observer.value()->query("SELECT * FROM state_journal");
                REQUIRE(rows.hasValue());
                CHECK(rows.value().empty());
                REQUIRE(kernel.shutdown().hasValue());
            }
            {
                AppKernel kernel;
                configurePersistence(kernel, path);
                configureRuntime(kernel, project, document, session, false);
                REQUIRE(kernel.bootstrap().hasValue());
                CHECK(kernel.documents().snapshot(document).value().objects().empty());
                CHECK(kernel.history().snapshot(document).value().cursor == HistoryCursor{});
                REQUIRE(kernel.execution().executeCommand(command).hasValue());
                CHECK(kernel.persistence().journalAfter(document, 0U).value().size() == 1U);
                REQUIRE(kernel.shutdown().hasValue());
            }
            removeDatabase(path);
        }
    }
}

TEST_CASE("Snapshot publication failures preserve the indexed state and permit orphan reuse", "[snapshot][persistence][fault-matrix]")
{
    using namespace lasercnc::test;
    for(const std::string stage : {"hash", "begin", "journal-query", "write-before", "write-after", "index", "commit", "existing-read", "existing-hash"}) {
        for(const bool throws : {false, true}) {
            DYNAMIC_SECTION(stage << " throws=" << throws) {
                const auto path = databasePath();
                const auto directory = std::filesystem::path{path.string() + ".snapshots"};
                const auto project = validId<ProjectId>("project.snapshot-fault");
                const auto document = validId<DocumentId>("document.snapshot-fault");
                const auto session = validId<SessionId>("session.snapshot-fault");
                const auto baseline = validId<SnapshotId>("snapshot.baseline");
                const auto candidate = validId<SnapshotId>("snapshot.candidate");
                RevisionSet revisions;
                std::vector<ObjectRecord> objects;
                std::optional<Document> imageForReuse;
                {
                    AppKernel kernel;
                    auto sqlite = SqlitePersistenceBackend::open({path});
                    REQUIRE(sqlite.hasValue());
                    auto backend = std::make_unique<FaultInjectingBackend>(std::move(sqlite).value());
                    auto* db = backend.get();
                    auto files = FilesystemSnapshotStore::create({directory, 1024U * 1024U});
                    REQUIRE(files.hasValue());
                    auto snapshots = std::make_unique<FaultSnapshotStore>(std::move(files).value());
                    auto* data = snapshots.get();
                    auto hashes = std::make_shared<FaultHashService>(std::make_shared<Sha256HashService>());
                    lasercnc::persistence::PersistenceService fixture;
                    REQUIRE(fixture.configure(std::move(backend), std::make_shared<JsonconsAdapter>(),
                        hashes, std::move(snapshots)).hasValue());
                    REQUIRE(fixture.initialize().hasValue());
                    // The seed Kernel owns a private database; only the fixture owns the fault database.
                    // 中文翻译：种子 Kernel 使用私有数据库，仅故障夹具拥有待注入的文件数据库。
                    auto kernelBackend = SqlitePersistenceBackend::open({":memory:"});
                    auto kernelFiles = FilesystemSnapshotStore::create({directory, 1024U * 1024U});
                    REQUIRE(kernelBackend.hasValue());
                    REQUIRE(kernelFiles.hasValue());
                    REQUIRE(kernel.configurePersistence(std::move(kernelBackend).value(),
                        std::make_shared<JsonconsAdapter>(), std::make_shared<Sha256HashService>(),
                        std::move(kernelFiles).value()).hasValue());
                    configureRuntime(kernel, project, document, session, true);
                    REQUIRE(kernel.bootstrap().hasValue());
                    const auto seeded = kernel.execution().executeCommand(request("request.snapshot.seed", "kernel.history.create",
                        project, document, session, "object.snapshot"));
                    REQUIRE(seeded.hasValue());
                    REQUIRE(seeded.value().commit.has_value());
                    REQUIRE(fixture.append(*seeded.value().commit).hasValue());
                    REQUIRE(fixture.saveDocumentLifecycle(project, document,
                        lasercnc::persistence::DocumentPersistenceState::Open).hasValue());
                    const auto image = kernel.documents().snapshot(document).value();
                    revisions = image.revisions();
                    objects = image.objects().all();
                    REQUIRE(fixture.captureSnapshot(baseline, image).hasValue());
                    if(stage == "hash") { hashes->arm("lasercnc.document-snapshot", 1U, throws); }
                    if(stage == "begin") { db->arm(BackendPoint::Begin, "", 1U, throws); }
                    if(stage == "journal-query") { db->arm(BackendPoint::Query, "FROM state_journal WHERE document_id", 1U, throws); }
                    if(stage == "write-before") { data->arm(SnapshotFault::BeforeWrite, throws); }
                    if(stage == "write-after") { data->arm(SnapshotFault::AfterWrite, throws); }
                    if(stage == "index") { db->arm(BackendPoint::Execute, "INSERT INTO snapshot_index", 1U, throws); }
                    if(stage == "commit") { db->arm(BackendPoint::Commit, "", 1U, throws); }
                    if(stage == "existing-read") { data->arm(SnapshotFault::Read, throws); }
                    if(stage == "existing-hash") { hashes->arm("lasercnc.document-snapshot", 2U, throws); }
                    const auto target = stage.starts_with("existing-") ? baseline : candidate;
                    REQUIRE_FALSE(fixture.captureSnapshot(target, image).hasValue());
                    CHECK(db->hits + data->hits + hashes->hits == 1U);
                    auto indexed = db->query("SELECT * FROM snapshot_index");
                    REQUIRE(indexed.hasValue());
                    CHECK(indexed.value().size() == 1U);
                    CHECK(fixture.latestSnapshot(document).value()->snapshotId == baseline);
                    CHECK(kernel.documents().snapshot(document).value().objects().all() == objects);
                    CHECK(kernel.documents().snapshot(document).value().revisions() == revisions);
                    CHECK(kernel.history().snapshot(document).value().cursor == HistoryCursor{1U, 1U});
                    CHECK(fixture.journalAfter(document, 0U).value().size() == 1U);
                    CHECK(std::filesystem::exists(directory / "snapshot.candidate.snapshot") ==
                        (stage == "write-after" || stage == "index" || stage == "commit"));
                    REQUIRE(kernel.shutdown().hasValue());
                }
                {
                    AppKernel kernel;
                    auto sqlite = SqlitePersistenceBackend::open({path});
                    REQUIRE(sqlite.hasValue());
                    auto files = FilesystemSnapshotStore::create({directory, 1024U * 1024U});
                    REQUIRE(files.hasValue());
                    REQUIRE(kernel.configurePersistence(std::move(sqlite).value(), std::make_shared<JsonconsAdapter>(),
                        std::make_shared<Sha256HashService>(), std::move(files).value()).hasValue());
                    configureRuntime(kernel, project, document, session, false);
                    REQUIRE(kernel.bootstrap().hasValue());
                    const auto image = kernel.documents().snapshot(document).value();
                    CHECK(image.objects().all() == objects);
                    CHECK(image.revisions() == revisions);
                    CHECK(kernel.history().snapshot(document).value().cursor == HistoryCursor{1U, 1U});
                    imageForReuse = image;
                    REQUIRE(kernel.shutdown().hasValue());
                }
                {
                    auto fixture = lasercnc::test::openPersistenceFixture(path, directory);
                    REQUIRE(imageForReuse.has_value());
                    REQUIRE(fixture->captureSnapshot(candidate, *imageForReuse).hasValue());
                    REQUIRE(fixture->captureSnapshot(candidate, *imageForReuse).hasValue());
                    CHECK(fixture->latestSnapshot(document).value()->snapshotId == candidate);
                }
                removeDatabase(path);
                std::filesystem::remove_all(directory);
            }
        }
    }
}

TEST_CASE("Close metadata faults retain committed data without claiming a detached document", "[document][snapshot][fault-matrix]")
{
    using namespace lasercnc::test;
    for(const bool failIndex : {false, true}) {
        for(const bool throws : {false, true}) {
            DYNAMIC_SECTION("failIndex=" << failIndex << " throws=" << throws) {
                const auto path = databasePath();
                const auto directory = std::filesystem::path{path.string() + ".snapshots"};
                const auto project = validId<ProjectId>("project.close-fault");
                const auto document = validId<DocumentId>("document.close-fault");
                const auto session = validId<SessionId>("session.close-fault");
                RevisionSet revisions;
                std::vector<ObjectRecord> objects;
                const auto setup = [&](AppKernel& kernel, std::unique_ptr<lasercnc::platform::IPersistenceBackend> backend, bool add) {
                    auto files = FilesystemSnapshotStore::create({directory, 1024U * 1024U});
                    REQUIRE(files.hasValue());
                    REQUIRE(kernel.configurePersistence(std::move(backend), std::make_shared<JsonconsAdapter>(),
                        std::make_shared<Sha256HashService>(), std::move(files).value()).hasValue());
                    configureRuntime(kernel, project, document, session, add);
                    REQUIRE(kernel.bootstrap().hasValue());
                };
                {
                    AppKernel kernel;
                    auto sqlite = SqlitePersistenceBackend::open({path});
                    REQUIRE(sqlite.hasValue());
                    auto backend = std::make_unique<FaultInjectingBackend>(std::move(sqlite).value());
                    auto* db = backend.get();
                    setup(kernel, std::move(backend), true);
                    REQUIRE(kernel.execution().executeCommand(request("request.close-fault", "kernel.history.create",
                        project, document, session, "object.close-fault")).hasValue());
                    const auto before = kernel.documents().snapshot(document).value();
                    objects = before.objects().all();
                    revisions = before.revisions();
                    db->arm(BackendPoint::Execute, failIndex ? "INSERT INTO snapshot_index" : "INSERT INTO document_catalog",
                        failIndex ? 1U : 2U, throws);
                    REQUIRE_FALSE(kernel.documentRuntime().close(document).hasValue());
                    CHECK(db->hits == 1U);
                    CHECK(kernel.documentRuntime().lifecycle(document).value().state == DocumentLifecycleState::Failed);
                    CHECK(kernel.documents().snapshot(document).value().objects().all() == objects);
                    CHECK(kernel.documents().snapshot(document).value().revisions() == revisions);
                    CHECK(kernel.persistence().journalAfter(document, 0U).value().size() == 1U);
                    CHECK(kernel.persistence().latestSnapshot(document).value().has_value() == !failIndex);
                    CHECK(kernel.persistence().documentCatalog().value().front().state == lasercnc::persistence::DocumentPersistenceState::Failed);
                    auto rawCatalog = db->query("SELECT state FROM document_catalog");
                    REQUIRE(rawCatalog.hasValue());
                    CHECK(*rawCatalog.value().front().at("state").getIf<std::string>() == (failIndex ? "failed" : "closing"));
                    REQUIRE(kernel.shutdown().hasValue());
                }
                {
                    AppKernel kernel;
                    auto sqlite = SqlitePersistenceBackend::open({path});
                    REQUIRE(sqlite.hasValue());
                    setup(kernel, std::move(sqlite).value(), false);
                    CHECK_FALSE(kernel.documents().contains(document));
                    CHECK(kernel.documentRuntime().lifecycle(document).value().state == DocumentLifecycleState::Failed);
                    CHECK_FALSE(kernel.documentRuntime().open(document).hasValue());
                    auto recovered = kernel.persistence().recover();
                    REQUIRE(recovered.hasValue());
                    REQUIRE(recovered.value().documents.size() == 1U);
                    CHECK(recovered.value().documents.front().objects == objects);
                    CHECK(recovered.value().documents.front().revisions == revisions);
                    REQUIRE(kernel.shutdown().hasValue());
                }
                removeDatabase(path);
                std::filesystem::remove_all(directory);
            }
        }
    }
}

TEST_CASE("Journal and outcome hash faults leave no partial history or idempotency", "[history][hash][fault-matrix]")
{
    for(const std::string stage : {"journal-digest", "journal-verify", "outcome-digest"}) {
        for(const bool throws : {false, true}) {
            DYNAMIC_SECTION(stage << " throws=" << throws) {
                const auto path = databasePath();
                const auto project = validId<ProjectId>("project.hash-fault");
                const auto document = validId<DocumentId>("document.hash-fault");
                const auto session = validId<SessionId>("session.hash-fault");
                auto command = request("request.hash-fault", "kernel.history.create", project, document, session, "object.hash");
                command.idempotencyKey = validId<IdempotencyKey>("key.hash-fault");
                {
                    AppKernel kernel;
                    auto sqlite = SqlitePersistenceBackend::open({path});
                    REQUIRE(sqlite.hasValue());
                    auto hashes = std::make_shared<lasercnc::test::FaultHashService>(std::make_shared<Sha256HashService>());
                    REQUIRE(kernel.configurePersistence(std::move(sqlite).value(), std::make_shared<JsonconsAdapter>(), hashes).hasValue());
                    configureRuntime(kernel, project, document, session, true);
                    REQUIRE(kernel.bootstrap().hasValue());
                    hashes->arm(stage == "outcome-digest" ? "lasercnc.command-outcome" : "lasercnc.state-journal",
                        stage == "journal-verify" ? 2U : 1U, throws);
                    REQUIRE_FALSE(kernel.execution().executeCommand(command).hasValue());
                    CHECK(hashes->hits == 1U);
                    CHECK(kernel.documents().snapshot(document).value().objects().empty());
                    CHECK(kernel.documents().snapshot(document).value().revisions() == RevisionSet{});
                    CHECK(kernel.history().snapshot(document).value().cursor == HistoryCursor{});
                    CHECK(kernel.persistence().journalAfter(document, 0U).value().empty());
                    auto observer = SqlitePersistenceBackend::open({path});
                    REQUIRE(observer.hasValue());
                    CHECK(observer.value()->query("SELECT * FROM command_idempotency").value().empty());
                    REQUIRE(kernel.shutdown().hasValue());
                }
                {
                    AppKernel kernel;
                    configurePersistence(kernel, path);
                    configureRuntime(kernel, project, document, session, false);
                    REQUIRE(kernel.bootstrap().hasValue());
                    CHECK(kernel.documents().snapshot(document).value().objects().empty());
                    auto retried = kernel.execution().executeCommand(command);
                    REQUIRE(retried.hasValue());
                    CHECK_FALSE(retried.value().replayed);
                    CHECK(kernel.execution().executeCommand(command).value().replayed);
                    CHECK(kernel.persistence().journalAfter(document, 0U).value().size() == 1U);
                    REQUIRE(kernel.shutdown().hasValue());
                }
                removeDatabase(path);
            }
        }
    }
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

TEST_CASE("Version three journals preserve versioned history without asset fields", "[history][persistence][compatibility]")
{
    const auto path = databasePath();
    const auto project = validId<ProjectId>("project.history");
    const auto document = validId<DocumentId>("document.history");
    const auto session = validId<SessionId>("session.history");
    {
        AppKernel kernel;
        configurePersistence(kernel, path);
        configureRuntime(kernel, project, document, session, true);
        REQUIRE(kernel.bootstrap().hasValue());
        REQUIRE(kernel.execution().executeCommand(request(
            "request.history.v3", "kernel.history.create", project, document, session, "object.history.v3")).hasValue());
        REQUIRE(kernel.shutdown().hasValue());
    }
    rewriteJournal(path, "transaction.request.history.v3", [](Value::Object& root) {
        root.at("version") = Value{std::int64_t{3}};
        for(auto& change : *root.at("changes").getIf<Value::Array>()) {
            for(const auto* name : {"before", "after"}) {
                if(auto* record = change.getIf<Value::Object>()->at(name).getIf<Value::Object>()) {
                    record->erase("assets");
                }
            }
        }
    });
    {
        AppKernel kernel;
        configurePersistence(kernel, path);
        configureRuntime(kernel, project, document, session, false);
        REQUIRE(kernel.bootstrap().hasValue());
        REQUIRE(kernel.execution().executeCommand(request(
            "request.history.v3.undo", "edit.undo", project, document, session)).hasValue());
        CHECK_FALSE(hasObject(kernel, document, "object.history.v3"));
        REQUIRE(kernel.shutdown().hasValue());
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
            for(auto& change : *root.at("changes").getIf<Value::Array>()) {
                auto& fields = *change.getIf<Value::Object>();
                for(const auto* name : {"before", "after"}) {
                    if(auto* object = fields.at(name).getIf<Value::Object>()) {
                        object->erase("schemaVersion");
                        object->erase("assets");
                    }
                }
            }
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
