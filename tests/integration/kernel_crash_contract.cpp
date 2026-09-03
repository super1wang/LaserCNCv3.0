#include "kernel_crash_contract.hpp"
#include "kernel_test_module.hpp"

#include <lasercnc/infrastructure/filesystem_snapshot_store.hpp>
#include <lasercnc/infrastructure/jsoncons_adapter.hpp>
#include <lasercnc/infrastructure/sha256_hash_service.hpp>
#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>

#include <array>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using namespace lasercnc;
using namespace foundation;
using namespace kernel;
using namespace runtime;
using namespace state;
using namespace infrastructure;

void check(bool condition, const char* message)
{
    if(!condition) { throw std::runtime_error(message); }
}

template<typename T> T take(Result<T> result)
{
    if(!result) { throw std::runtime_error(std::string(result.error().code.value()) + ": " + result.error().message); }
    return std::move(result).value();
}
void take(Result<void> result)
{
    if(!result) { throw std::runtime_error(std::string(result.error().code.value()) + ": " + result.error().message); }
}
template<typename Id> Id id(const char* value) { return take(Id::create(value)); }

const auto project = id<ProjectId>("project.crash");
const auto document = id<DocumentId>("document.crash");
const auto session = id<SessionId>("session.crash");
const auto key = id<IdempotencyKey>("key.crash.target");

struct CrashControl {
    std::string scenario;
    bool armed{false};
    std::function<void()> verifyBeforeExit;
    [[noreturn]] void terminate(const char* point) const
    {
        if(verifyBeforeExit) { verifyBeforeExit(); }
        std::cout << "crash-point:" << scenario << ':' << point << '\n' << std::flush;
        std::_Exit(86);
    }
};

class CrashBackend final : public platform::IPersistenceBackend {
public:
    CrashBackend(std::unique_ptr<platform::IPersistenceBackend> backend, CrashControl& control)
        : backend_(std::move(backend)), control_(control) {}
    Result<std::size_t> execute(std::string_view sql, std::span<const Value> values = {}) override
    {
        auto result = backend_->execute(sql, values);
        if(result && sql.starts_with("INSERT INTO state_journal")) {
            journalWritten_ = true;
            if(control_.armed && control_.scenario.ends_with("inserted")) { control_.terminate("journal-inserted"); }
        }
        if(result && control_.armed && control_.scenario == "outcome-written"
           && sql.starts_with("UPDATE command_idempotency SET status='completed'")) {
            control_.terminate("outcome-written");
        }
        return result;
    }
    Result<std::vector<platform::PersistenceRow>> query(std::string_view sql, std::span<const Value> values = {}) override
    { return backend_->query(sql, values); }
    Result<void> beginTransaction() override
    { journalWritten_ = false; return backend_->beginTransaction(); }
    Result<void> commitTransaction() override
    {
        if(control_.armed && journalWritten_ && control_.scenario == "transaction-before-commit") {
            control_.terminate("before-commit");
        }
        auto committed = backend_->commitTransaction();
        if(committed && control_.armed && journalWritten_ && control_.scenario.ends_with("committed")) {
            control_.terminate("after-commit-before-memory");
        }
        return committed;
    }
    Result<void> rollbackTransaction() override { return backend_->rollbackTransaction(); }
private:
    std::unique_ptr<platform::IPersistenceBackend> backend_;
    CrashControl& control_;
    bool journalWritten_{false};
};

class NullLog final : public observability::ILogService {
public:
    Result<void> write(const observability::LogRecord&) override { return Result<void>::success(); }
    Result<void> flush() override { return Result<void>::success(); }
};

class CreateHandler final : public ICommandHandler {
public:
    explicit CreateHandler(CrashControl& control) : control_(control) {}
    Result<Value> execute(const CommandRequest& request, ApplicationTransaction& transaction) override
    {
        ++calls;
        const auto& name = *request.arguments.getIf<Value::Object>()->at("id").getIf<std::string>();
        take(transaction.createObject({take(ObjectId::create(name)), id<ObjectTypeId>("type.crash"), Value{name}}));
        take(transaction.touchRevision(RevisionScope::Geometry));
        if(control_.armed && control_.scenario == "command-staged") { control_.terminate("handler-staged"); }
        return Result<Value>::success(Value{Value::Object{{"id", Value{name}}}});
    }
    unsigned int calls{0U};
private:
    CrashControl& control_;
};

CommandRequest createRequest(bool baseline = false)
{
    CommandRequest request{id<RequestId>(baseline ? "request.crash.baseline" : "request.crash.target"),
        {session, project, document}, id<CommandName>("test.crash.create"), {1U, 0U, 0U},
        Value{Value::Object{{"id", Value{baseline ? "object.baseline" : "object.target"}}}}, std::nullopt,
        id<CorrelationId>("correlation.crash"), id<TraceId>("trace.crash")};
    if(!baseline) { request.idempotencyKey = key; }
    return request;
}
CommandRequest historyRequest(bool undo, const char* phase = "recover")
{
    return {take(RequestId::create(std::string("request.crash.") + phase + (undo ? "-undo" : "-redo"))),
        {session, project, document}, id<CommandName>(undo ? "edit.undo" : "edit.redo"), {1U, 0U, 0U},
        Value{Value::Object{}}, std::nullopt, id<CorrelationId>("correlation.crash"), id<TraceId>("trace.crash")};
}

std::shared_ptr<CreateHandler> configure(AppKernel& kernel, const std::filesystem::path& root,
    CrashControl& control, bool seed)
{
    if(seed) { take(kernel.addDocument(project, document)); }
    take(kernel.executionServices().configure(std::make_shared<JsonconsAdapter>(), std::make_shared<NullLog>()));
    take(test::registerObjectType(kernel, test::valueObjectType("type.crash")));
    auto handler = std::make_shared<CreateHandler>(control);
    auto schema = take(Schema::create(id<SchemaId>("schema.crash"), {1U, 0U, 0U}, SchemaKind::Object));
    take(test::registerCommand(kernel, CommandDescriptor{id<CommandName>("test.crash.create"), {1U, 0U, 0U},
        schema, schema, ExecutionMode::Synchronous, SideEffectLevel::DocumentWrite,
        id<CapabilityId>("document.write"), true, true, true}, handler));
    take(kernel.capabilities().replace(session, std::array{id<CapabilityId>("document.write"), id<CapabilityId>("kernel.history.edit")}));
    auto backend = take(SqlitePersistenceBackend::open({root / "state.db"}));
    auto store = take(FilesystemSnapshotStore::create({root / "snapshots", 1024U * 1024U}));
    take(kernel.persistence().configure(std::make_unique<CrashBackend>(std::move(backend), control),
        std::make_shared<JsonconsAdapter>(), std::make_shared<Sha256HashService>(), std::move(store)));
    take(kernel.bootstrap());
    return handler;
}

struct Expected { std::uint64_t revision; std::size_t position; std::size_t extent; bool target; };

void verify(AppKernel& kernel, Expected expected)
{
    const auto image = take(kernel.documents().snapshot(document));
    check(image.projectId() == project, "Document ownership drift");
    check(image.objects().size() == (expected.target ? 2U : 1U), "Object count drift");
    check(image.objects().contains(id<ObjectId>("object.target")) == expected.target, "Target existence drift");
    for(const auto& object : image.objects().all()) {
        check(object.type == id<ObjectTypeId>("type.crash") && object.schemaVersion == Version{1U, 0U, 0U}
            && object.assets.empty() && object.data == Value{std::string(object.id.value())}, "Object material drift");
    }
    for(const auto scope : {RevisionScope::Project, RevisionScope::Document, RevisionScope::Geometry}) {
        check(image.revisions().at(scope) == Revision{expected.revision}, "Revision drift");
    }
    for(const auto scope : {RevisionScope::Cam, RevisionScope::MachineContext, RevisionScope::Environment}) {
        check(image.revisions().at(scope) == Revision{}, "Unrelated revision changed");
    }
    const auto history = take(kernel.history().snapshot(document));
    check(history.cursor == HistoryCursor{expected.position, expected.extent}, "History cursor drift");
    check(history.entries.size() == expected.extent && !history.barrier.has_value(), "History material drift");
    const auto journal = take(kernel.persistence().journalAfter(document, 0U));
    check(journal.size() == expected.revision, "Journal count drift");
    for(std::size_t index = 0U; index < journal.size(); ++index) {
        check(journal[index].revisionsBefore.at(RevisionScope::Document) == Revision{index}
            && journal[index].revisionsAfter.at(RevisionScope::Document) == Revision{index + 1U}, "Journal chain drift");
    }
}

Expected beforeRecovery(std::string_view scenario)
{
    if(scenario == "undo-inserted") { return {2U, 2U, 2U, true}; }
    if(scenario == "undo-committed" || scenario == "redo-inserted") { return {3U, 1U, 2U, false}; }
    if(scenario == "redo-committed") { return {4U, 2U, 2U, true}; }
    if(scenario == "transaction-committed" || scenario == "command-returned") { return {2U, 2U, 2U, true}; }
    return {1U, 1U, 1U, false};
}

Expected afterRecovery(std::string_view scenario)
{
    if(scenario.starts_with("undo-")) { return {4U, 2U, 2U, true}; }
    if(scenario.starts_with("redo-")) { return {5U, 1U, 2U, false}; }
    return {2U, 2U, 2U, true};
}

} // namespace

int runKernelCrashContract(std::string_view mode, const std::filesystem::path& root, std::string_view scenario)
{
    const std::array<std::string_view, 10> scenarios{"command-staged", "journal-inserted", "outcome-written",
        "transaction-before-commit", "transaction-committed", "command-returned",
        "undo-inserted", "undo-committed", "redo-inserted", "redo-committed"};
    check(std::find(scenarios.begin(), scenarios.end(), scenario) != scenarios.end(), "Unknown crash scenario");
    check(root.is_absolute(), "Crash contract requires an absolute isolated state root");
    check(mode == "crash-seed" || mode == "crash-recover" || mode == "crash-audit", "Unknown crash mode");
    const bool seed = mode == "crash-seed";
    if(seed) {
        std::filesystem::create_directories(root);
        check(std::filesystem::is_empty(root), "Refusing to seed a nonempty crash state root");
    } else {
        check(std::filesystem::is_regular_file(root / "state.db"), "Crash database is missing");
    }
    CrashControl control{std::string(scenario), false, {}};
    AppKernel kernel;
    const auto handler = configure(kernel, root, control, seed);
    if(seed) {
        take(kernel.execution().executeCommand(createRequest(true)));
        take(kernel.persistence().captureSnapshot(id<SnapshotId>("snapshot.crash.baseline"), take(kernel.documents().snapshot(document))));
        if(scenario.starts_with("undo-") || scenario.starts_with("redo-")) {
            take(kernel.execution().executeCommand(createRequest()));
        }
        if(scenario.starts_with("redo-")) { take(kernel.execution().executeCommand(historyRequest(true, "seed"))); }
        if(scenario != "command-returned") {
            const auto before = take(kernel.documents().snapshot(document));
            control.verifyBeforeExit = [&kernel, before] {
                const auto current = take(kernel.documents().snapshot(document));
                check(current.revisions() == before.revisions() && current.objects().all() == before.objects().all(),
                    "The document changed before the selected crash boundary");
            };
        }
        control.armed = true;
        take(kernel.execution().executeCommand(scenario.starts_with("undo-") ? historyRequest(true, "crash")
            : scenario.starts_with("redo-") ? historyRequest(false, "crash") : createRequest()));
        if(scenario == "command-returned") { verify(kernel, {2U, 2U, 2U, true}); control.terminate("command-returned"); }
        throw std::runtime_error("Crash point was not reached");
    }
    if(mode == "crash-audit") {
        verify(kernel, afterRecovery(scenario));
        check(handler->calls == 0U, "Audit executed a handler");
        take(kernel.shutdown());
        std::cout << "crash-audited:" << scenario << '\n';
        return 0;
    }
    const auto expected = beforeRecovery(scenario);
    verify(kernel, expected);
    auto observer = take(SqlitePersistenceBackend::open({root / "state.db"}));
    const auto claims = take(observer->query("SELECT status FROM command_idempotency"));
    check(claims.size() == 1U, "Idempotency claim missing or duplicated");
    const bool committed = expected.extent == 2U;
    check(claims.front().at("status") == Value{committed ? "completed" : "abandoned"}, "Idempotency recovery disposition drift");
    if(scenario.starts_with("undo-")) {
        if(scenario == "undo-inserted") { take(kernel.execution().executeCommand(historyRequest(true))); }
        take(kernel.execution().executeCommand(historyRequest(false)));
    } else if(scenario.starts_with("redo-")) {
        if(scenario == "redo-inserted") { take(kernel.execution().executeCommand(historyRequest(false))); }
        take(kernel.execution().executeCommand(historyRequest(true)));
    } else {
        const auto response = take(kernel.execution().executeCommand(createRequest()));
        check(response.replayed == committed, "Unexpected idempotency replay disposition");
        check(handler->calls == (committed ? 0U : 1U), "Unexpected handler execution count");
    }
    const auto replay = take(kernel.execution().executeCommand(createRequest()));
    check(replay.replayed, "Completed command was not replayed");
    check(replay.result == Value{Value::Object{{"id", Value{"object.target"}}}}
        && replay.resolvedVersion == Version{1U, 0U, 0U}, "Replayed receipt drift");
    check(handler->calls == (committed ? 0U : 1U), "Recovery unexpectedly repeated a handler");
    verify(kernel, afterRecovery(scenario));
    take(kernel.shutdown());
    std::cout << "crash-recovered:" << scenario << '\n';
    return 0;
}
