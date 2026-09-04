#include "kernel_crash_contract.hpp"
#include "kernel_test_module.hpp"
#include "persistence_fixture.hpp"
#include "kernel_file_crash_probe.hpp"

#include <lasercnc/infrastructure/filesystem_snapshot_store.hpp>
#include <lasercnc/infrastructure/filesystem_asset_store.hpp>
#include <lasercnc/infrastructure/bs_thread_pool_executor.hpp>
#include <lasercnc/infrastructure/jsoncons_adapter.hpp>
#include <lasercnc/infrastructure/sha256_hash_service.hpp>
#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>

#include <array>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <functional>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

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

const std::string assetContent{"immutable crash asset\0binary\xff", 29U};
std::span<const std::byte> assetBytes()
{
    return {reinterpret_cast<const std::byte*>(assetContent.data()), assetContent.size()};
}
AssetRef expectedAsset()
{
    const auto kind = id<AssetKind>("test.crash.binary");
    const auto digest = take(Sha256HashService{}.digest(assetBytes()));
    const std::string identity = "LCNCAssetRef1\n" + std::to_string(kind.value().size()) + ':'
        + std::string(kind.value()) + '\n' + std::string(digest.value()) + ':' + std::to_string(assetContent.size());
    const auto identityDigest = take(Sha256HashService{}.digest(
        {reinterpret_cast<const std::byte*>(identity.data()), identity.size()}));
    return {take(AssetId::create("asset.sha256." + std::string(identityDigest.value().substr(7U)))),
        digest, kind, assetContent.size()};
}

struct CrashControl {
    std::string scenario;
    bool armed{false};
    std::function<void()> verifyBeforeExit;
    std::filesystem::path root;
    std::thread::id caller{std::this_thread::get_id()};
    std::shared_ptr<platform::IAssetStore> assets;
    void recordCall(const char* category) const
    {
        std::ofstream stream(root / "execution-calls.log", std::ios::app);
        stream << category << '\n';
        stream.close();
        check(!stream.fail(), "Cannot persist independent handler call record");
    }
    std::size_t callCount(const char* category) const
    {
        std::ifstream stream(root / "execution-calls.log");
        check(stream.is_open(), "Independent handler call record missing");
        std::size_t count = 0U;
        for(std::string line; std::getline(stream, line);) {
            check(line == category, "Unexpected handler call category");
            ++count;
        }
        check(stream.eof(), "Cannot read independent handler call record");
        return count;
    }
    [[noreturn]] void terminate(const char* point) const
    {
        if(verifyBeforeExit) { verifyBeforeExit(); }
        std::cout << "crash-point:" << scenario << ':' << point << '\n' << std::flush;
        std::_Exit(86);
    }
};

class CrashBackend final : public platform::IPersistenceBackend {
public:
    Result<platform::PersistenceSessionInfo> acquireHostSession() override
    { return backend_->acquireHostSession(); }
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
        if(name == "object.target" && control_.scenario.starts_with("workflow-")) { control_.recordCall("document"); }
        std::vector<AssetRef> assets;
        if(name == "object.target" && control_.scenario.starts_with("asset-")) {
            if(control_.armed && control_.scenario == "asset-before-write") { control_.terminate("asset-before-write"); }
            assets.push_back(take(control_.assets->publish(expectedAsset().kind, assetBytes())));
            check(assets.front() == expectedAsset(), "Published asset identity drift");
            if(control_.armed && control_.scenario == "asset-published") { control_.terminate("asset-published"); }
        }
        take(transaction.createObject({take(ObjectId::create(name)), id<ObjectTypeId>("type.crash"), Value{name},
            {1U, 0U, 0U}, std::move(assets)}));
        take(transaction.touchRevision(RevisionScope::Geometry));
        if(control_.armed && (control_.scenario == "command-staged" || control_.scenario == "workflow-handler")) {
            control_.terminate("handler-staged");
        }
        return Result<Value>::success(Value{Value::Object{{"id", Value{name}}}});
    }
    unsigned int calls{0U};
private:
    CrashControl& control_;
};

class CrashTaskHandler final : public ITaskHandler {
public:
    explicit CrashTaskHandler(CrashControl& control) : control_(control) {}
    Result<Value> execute(const TaskRequest&, const TaskContext& context) override
    {
        check(std::this_thread::get_id() != control_.caller, "Task did not run on the real executor worker");
        check(context.document.has_value() && context.document->id() == document
            && context.document->revisions().at(RevisionScope::Document) == Revision{1U}
            && context.resources.claims.size() == 1U, "Task execution context drift");
        take(context.progress.report(0.5, "crash boundary"));
        control_.recordCall("task");
        if(control_.armed) { control_.terminate("task-handler"); }
        return Result<Value>::success(Value{Value::Object{}});
    }
private:
    CrashControl& control_;
};

ReplayPolicy effectPolicy(std::string_view scenario)
{
    if(scenario.ends_with("safe")) { return ReplayPolicy::Safe; }
    if(scenario.ends_with("idempotent")) { return ReplayPolicy::Idempotent; }
    if(scenario.ends_with("reconcile")) { return ReplayPolicy::ReconcileOnly; }
    return ReplayPolicy::Never;
}

class AllowTestEffectGuard final : public IEffectGuard {
public:
    Result<void> evaluate(const CommandRequest&, const CommandDescriptor&, const EffectGuardContext&) override
    { return Result<void>::success(); }
};

class CrashEffectHandler final : public IExternalEffectHandler {
public:
    explicit CrashEffectHandler(CrashControl& control) : control_(control) {}
    Result<Value> execute(const CommandRequest&, const ExternalEffectContext& context) override
    {
        check(context.document.has_value() && context.resources.claims.size() == 1U, "Effect context drift");
        check(context.resumed == !control_.armed, "Effect resumed flag drift");
        control_.recordCall("effect");
        if(control_.armed) { control_.terminate("effect-handler"); }
        return Result<Value>::success(Value{Value::Object{{"published", Value{true}}}});
    }
private:
    CrashControl& control_;
};

WorkflowRequest workflowRequest()
{
    return {id<WorkflowId>("workflow.crash.instance"), id<WorkflowName>("workflow.crash"), Value{Value::Object{}},
        session, project, document, id<CorrelationId>("correlation.crash"), id<TraceId>("trace.crash")};
}

CommandRequest createRequest(bool baseline = false)
{
    CommandRequest request{id<RequestId>(baseline ? "request.crash.baseline" : "request.crash.target"),
        {session, project, document}, id<CommandName>("test.crash.create"), {1U, 0U, 0U},
        Value{Value::Object{{"id", Value{baseline ? "object.baseline" : "object.target"}}}}, std::nullopt,
        id<CorrelationId>("correlation.crash"), id<TraceId>("trace.crash")};
    if(!baseline) { request.idempotencyKey = key; }
    return request;
}
CommandRequest executionRequest(bool task)
{
    auto request = createRequest();
    request.command = id<CommandName>(task ? "test.crash.task" : "test.crash.effect");
    request.arguments = Value{Value::Object{}};
    return request;
}
CommandRequest historyRequest(bool undo, const char* phase = "recover")
{
    return {take(RequestId::create(std::string("request.crash.") + phase + (undo ? "-undo" : "-redo"))),
        {session, project, document}, id<CommandName>(undo ? "edit.undo" : "edit.redo"), {1U, 0U, 0U},
        Value{Value::Object{}}, std::nullopt, id<CorrelationId>("correlation.crash"), id<TraceId>("trace.crash")};
}

std::shared_ptr<CreateHandler> configure(AppKernel& kernel, const std::filesystem::path& root,
    CrashControl& control, bool seed, Result<void>* startup = nullptr)
{
    if(control.scenario.starts_with("asset-")) {
        control.assets = take(FilesystemAssetStore::create({root / "assets", 4096U}, std::make_shared<Sha256HashService>()));
        take(kernel.configureAssetStore(control.assets));
        setKernelFilePublishProbe([&control](const auto& temporary, const auto& target, bool after) {
            if(!control.armed || target.parent_path() != control.root / "assets") { return; }
            if((!after && control.scenario == "asset-before-rename")
                || (after && control.scenario == "asset-after-rename")) {
                check(std::filesystem::exists(temporary) == !after && std::filesystem::exists(target) == after,
                    "Atomic publication file-state drift");
                check(std::filesystem::file_size(after ? target : temporary)
                    == 12U + expectedAsset().kind.value().size() + assetContent.size(), "Asset envelope incomplete at rename");
                control.terminate(after ? "asset-after-rename" : "asset-before-rename");
            }
        });
    }
    if(seed) { take(kernel.addDocument(project, document)); }
    take(kernel.executionServices().configure(std::make_shared<JsonconsAdapter>(), std::make_shared<NullLog>()));
    take(test::registerObjectType(kernel, test::valueObjectType("type.crash")));
    auto handler = std::make_shared<CreateHandler>(control);
    auto schema = take(Schema::create(id<SchemaId>("schema.crash"), {1U, 0U, 0U}, SchemaKind::Object));
    take(test::registerCommand(kernel, CommandDescriptor{id<CommandName>("test.crash.create"), {1U, 0U, 0U},
        schema, schema, ExecutionMode::Synchronous, SideEffectLevel::DocumentWrite,
        id<CapabilityId>("document.write"), true, true, true}, handler));
    take(kernel.capabilities().replace(session, std::array{id<CapabilityId>("document.write"),
        id<CapabilityId>("kernel.history.edit"), id<CapabilityId>("task.submit"), id<CapabilityId>("effect.publish")}));
    const auto resource = ResourceClaim{ResourceKind::DiskIO, id<ResourceId>("resource.crash"), ResourceAccess::Exclusive, 1U};
    if(control.scenario == "task-handler") {
        take(kernel.configureTaskExecutor(take(BsThreadPoolExecutor::create({1U}))));
        take(kernel.resources().configure(resource.kind, resource.resource, 1U));
        take(test::registerTask(kernel, TaskDescriptor{id<TaskName>("task.crash"), {1U, 0U, 0U}, schema, schema},
            std::make_shared<CrashTaskHandler>(control)));
        TaskRequest task{id<TaskId>("task.crash.instance"), id<TaskName>("task.crash"), Value{Value::Object{}},
            id<TraceId>("trace.crash"), id<CorrelationId>("correlation.crash"), project, document};
        task.resources = {resource};
        take(test::registerAsyncCommand(kernel, CommandDescriptor{id<CommandName>("test.crash.task"), {1U, 0U, 0U},
            schema, schema, ExecutionMode::Asynchronous, SideEffectLevel::ReadOnly, id<CapabilityId>("task.submit"),
            false, true, true}, std::make_shared<test::FixedTaskCommandHandler>(std::move(task))));
    }
    if(control.scenario.find("effect-") != std::string::npos) {
        take(kernel.resources().configure(resource.kind, resource.resource, 1U));
        take(kernel.effectGuards().registerGuard(id<EffectGuardId>("guard.crash"), std::make_shared<AllowTestEffectGuard>()));
        auto descriptor = CommandDescriptor{id<CommandName>("test.crash.effect"), {1U, 0U, 0U}, schema, schema,
            ExecutionMode::Synchronous, SideEffectLevel::Publish, id<CapabilityId>("effect.publish"), false, false, true};
        descriptor.replayPolicy = effectPolicy(control.scenario);
        descriptor.effectGuards = {id<EffectGuardId>("guard.crash")};
        descriptor.resources = {resource};
        take(test::registerExternalEffectCommand(kernel, descriptor, std::make_shared<CrashEffectHandler>(control)));
    }
    if(control.scenario.starts_with("workflow-")) {
        const bool effect = control.scenario.starts_with("workflow-effect-");
        WorkflowStep step{id<WorkflowStepId>("step.crash"), WorkflowStepKind::Command};
        step.command = WorkflowCommandCall{id<CommandName>(effect ? "test.crash.effect" : "test.crash.create"),
            {1U, 0U, 0U}, effect ? Value{Value::Object{}} : Value{Value::Object{{"id", Value{"object.target"}}}}};
        step.resultBinding = "result";
        take(test::registerWorkflow(kernel, WorkflowDefinition{
            WorkflowDescriptor{id<WorkflowName>("workflow.crash"), {1U, 0U, 0U}, schema, schema},
            {step}, Value{Value::Object{}}}));
    }
    auto backend = take(SqlitePersistenceBackend::open({root / "state.db"}));
    auto store = take(FilesystemSnapshotStore::create({root / "snapshots", 1024U * 1024U}));
    take(kernel.configurePersistence(std::make_unique<CrashBackend>(std::move(backend), control),
        std::make_shared<JsonconsAdapter>(), std::make_shared<Sha256HashService>(), std::move(store)));
    auto bootstrapped = kernel.bootstrap();
    if(startup) { *startup = std::move(bootstrapped); }
    else { take(std::move(bootstrapped)); }
    return handler;
}

struct Expected { std::uint64_t revision; std::size_t position; std::size_t extent; bool target; };

void verify(AppKernel& kernel, Expected expected, const std::vector<AssetRef>& assets = {})
{
    const auto image = take(kernel.documents().snapshot(document));
    check(image.projectId() == project, "Document ownership drift");
    check(image.objects().size() == (expected.target ? 2U : 1U), "Object count drift");
    check(image.objects().contains(id<ObjectId>("object.target")) == expected.target, "Target existence drift");
    for(const auto& object : image.objects().all()) {
        check(object.type == id<ObjectTypeId>("type.crash") && object.schemaVersion == Version{1U, 0U, 0U}
            && object.assets == (object.id == id<ObjectId>("object.target") ? assets : std::vector<AssetRef>{})
            && object.data == Value{std::string(object.id.value())}, "Object material drift");
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

bool executionScenario(std::string_view scenario)
{
    return scenario == "task-handler" || scenario.starts_with("workflow-") || scenario.starts_with("effect-");
}

bool containsError(const Error& error, const char* code)
{
    return error.code.value() == code || (error.cause && containsError(*error.cause, code));
}

void recoverExecution(AppKernel& kernel, CrashControl& control, const CreateHandler& handler, bool audit)
{
    const bool task = control.scenario == "task-handler";
    const bool workflow = control.scenario.starts_with("workflow-");
    const bool effect = control.scenario.find("effect-") != std::string::npos;
    const bool documentCommitted = control.scenario == "workflow-committed" || (audit && workflow && !effect);
    verify(kernel, documentCommitted ? Expected{2U, 2U, 2U, true} : Expected{1U, 1U, 1U, false});
    const bool retryAllowed = effect && explicitRetryAllowed(effectPolicy(control.scenario));
    const auto category = task ? "task" : effect ? "effect" : "document";
    const std::size_t finalCalls = retryAllowed || control.scenario == "workflow-handler" ? 2U : 1U;
    check(control.callCount(category) == (audit ? finalCalls : 1U), "Bootstrap repeated a handler");

    if(task) {
        const auto snapshot = take(kernel.execution().task(id<TaskId>("task.crash.instance")));
        check(snapshot.state == TaskState::Failed && snapshot.error
            && containsError(*snapshot.error, "Task.InterruptedByRestart") && !snapshot.result
            && snapshot.sourceRevisions == take(kernel.documents().snapshot(document)).revisions(),
            "Interrupted task history drift");
        check(kernel.scheduler().activeTaskCount() == 0U, "Recovery recreated an active task");
        const auto response = take(kernel.execution().executeCommand(executionRequest(true)));
        check(response.replayed && response.taskId == id<TaskId>("task.crash.instance"), "Task acceptance was not replayed");
        const auto waited = take(kernel.execution().waitTask(id<TaskId>("task.crash.instance"), std::chrono::milliseconds{100}));
        check(waited.state == TaskState::Failed && waited.error
            && containsError(*waited.error, "Task.InterruptedByRestart"), "Interrupted task wait restarted work");
    }

    auto observer = take(SqlitePersistenceBackend::open({control.root / "state.db"}));
    std::optional<IdempotencyKey> effectKey;
    const auto interruptedState = effectPolicy(control.scenario) == ReplayPolicy::Never ? ExternalEffectState::Indeterminate
        : effectPolicy(control.scenario) == ReplayPolicy::ReconcileOnly ? ExternalEffectState::ReconcileRequired
        : ExternalEffectState::Interrupted;
    if(effect) {
        const auto rows = take(observer->query("SELECT idempotency_key FROM external_effects"));
        check(rows.size() == 1U && rows.front().at("idempotency_key").getIf<std::string>(), "Effect record missing");
        effectKey = take(IdempotencyKey::create(*rows.front().at("idempotency_key").getIf<std::string>()));
        const auto record = take(kernel.persistence().externalEffect(*effectKey));
        check(record && record->state == (audit && retryAllowed ? ExternalEffectState::Completed : interruptedState),
            "Effect recovery disposition drift");
    }

    if(workflow) {
        const auto request = workflowRequest();
        const auto before = take(kernel.execution().workflow(request.workflowId));
        check(before.steps.size() == 1U && before.steps.front().attempt == 1U, "Workflow attempt drift");
        if(!audit) {
            check(before.state == WorkflowState::Waiting && before.steps.front().state == WorkflowStepState::Waiting
                && before.steps.front().replayCurrentAttempt, "Running workflow was not restored for the same attempt");
            const auto durable = take(kernel.persistence().workflowCheckpoint(request.workflowId));
            check(durable && durable->snapshot.state == WorkflowState::Running
                && durable->snapshot.steps.front().state == WorkflowStepState::Running, "Crash checkpoint was not Running");
        }
        const auto advanced = take(kernel.execution().advanceWorkflow(request.workflowId));
        check(advanced.steps.front().attempt == 1U && !advanced.steps.front().replayCurrentAttempt,
            "Workflow recovery started a new attempt");
        if(effect) {
            check(advanced.state == WorkflowState::Failed && advanced.steps.front().state == WorkflowStepState::Failed
                && advanced.steps.front().error && containsError(*advanced.steps.front().error,
                    interruptedState == ExternalEffectState::Indeterminate
                        ? "Persistence.ExternalEffectIndeterminate" : "Persistence.ExternalEffectReconcileRequired"),
                "Workflow replayed an unsafe effect or lost its recovery error");
        } else {
            check(advanced.state == WorkflowState::Succeeded && advanced.steps.front().state == WorkflowStepState::Succeeded
                && advanced.steps.front().result == Value{Value::Object{{"id", Value{"object.target"}}}},
                "Workflow did not reuse/finish the original document attempt");
        }
        const auto terminal = take(kernel.execution().advanceWorkflow(request.workflowId));
        check(terminal.state == advanced.state && terminal.steps.front().attempt == 1U, "Terminal workflow ran again");
    } else if(effect) {
        const auto response = kernel.execution().executeCommand(executionRequest(false));
        if(retryAllowed) {
            check(response.hasValue() && response.value().replayed == audit
                && response.value().recoveryDisposition == RecoveryDisposition::Completed, "Explicit effect retry disposition drift");
            const auto replay = take(kernel.execution().executeCommand(executionRequest(false)));
            check(replay.replayed && replay.result == Value{Value::Object{{"published", Value{true}}}}, "Completed effect ran again");
        } else {
            check(!response && containsError(response.error(), interruptedState == ExternalEffectState::Indeterminate
                ? "Persistence.ExternalEffectIndeterminate" : "Persistence.ExternalEffectReconcileRequired"),
                "Unsafe effect explicit retry was not blocked");
        }
    }
    if(effect) {
        const auto record = take(kernel.persistence().externalEffect(*effectKey));
        check(record && record->state == (retryAllowed ? ExternalEffectState::Completed : interruptedState),
            "Effect final disposition drift");
    }
    const auto claims = take(observer->query("SELECT status FROM command_idempotency"));
    check(claims.size() == (effect ? 0U : 1U), "Unexpected command idempotency record count");
    if(!claims.empty()) { check(claims.front().at("status") == Value{"completed"}, "Command receipt not completed"); }
    check(control.callCount(category) == finalCalls, "Handler count changed during recovery");
    check(handler.calls == (!audit && control.scenario == "workflow-handler" ? 1U : 0U), "Document handler unexpectedly ran");
    check(kernel.scheduler().activeTaskCount() == 0U, "Active task leaked after recovery");
    for(const auto& resource : kernel.resources().snapshot()) {
        check(!resource.exclusivelyHeld, "Recovered execution leaked its exclusive resource");
    }
    verify(kernel, workflow && !effect ? Expected{2U, 2U, 2U, true} : Expected{1U, 1U, 1U, false});
}

bool damagedAssetScenario(std::string_view scenario)
{
    return scenario == "asset-missing-committed" || scenario == "asset-corrupt-committed";
}

std::filesystem::path assetPath(const CrashControl& control)
{
    return control.root / "assets" / (std::string(expectedAsset().id.value()) + ".snapshot");
}

void damagePublishedAsset(const CrashControl& control)
{
    const auto target = assetPath(control);
    const auto evidence = control.root / "asset-original.snapshot";
    check(std::filesystem::is_regular_file(target) && !std::filesystem::exists(evidence), "Invalid damage fixture paths");
    if(control.scenario == "asset-missing-committed") {
        std::filesystem::rename(target, evidence);
    } else {
        std::filesystem::copy_file(target, evidence);
        std::fstream stream(target, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekp(-1, std::ios::end);
        stream.put('^');
        stream.close();
        check(!stream.fail(), "Cannot create corrupted asset fixture");
    }
}

void verifyAssetFiles(const CrashControl& control, bool published)
{
    const auto target = assetPath(control);
    std::size_t temporaryCount = 0U;
    std::size_t publishedCount = 0U;
    for(const auto& file : std::filesystem::directory_iterator(control.root / "assets")) {
        check(file.is_regular_file(), "Unexpected asset directory entry");
        if(file.path() == target) { ++publishedCount; }
        else {
            check(file.path().filename().string().starts_with(target.filename().string() + ".tmp."),
                "Unexpected file in asset storage");
            ++temporaryCount;
            std::ifstream stream(file.path(), std::ios::binary);
            check(stream.is_open(), "Temporary orphan is unreadable after process exit");
            const std::string payload{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
            check(!stream.bad() && payload.starts_with("LCNCAS01")
                && payload.size() == 12U + expectedAsset().kind.value().size() + assetContent.size()
                && payload.substr(12U + expectedAsset().kind.value().size()) == assetContent,
                "Flushed temporary orphan content drift");
        }
    }
    check(publishedCount == (published ? 1U : 0U), "Published asset file count drift");
    check(temporaryCount == (control.scenario == "asset-before-rename" ? 1U : 0U), "Temporary orphan state drift");
    const auto content = control.assets->read(expectedAsset());
    if(published) {
        check(content.hasValue() && content.value() == std::vector<std::byte>(assetBytes().begin(), assetBytes().end()),
            "Published asset content drift");
    } else {
        check(!content && containsError(content.error(), "Asset.NotFound"), "Unpublished temporary asset became readable");
    }
}

void recoverAsset(AppKernel& kernel, CrashControl& control, const CreateHandler& handler, bool audit)
{
    const bool committed = control.scenario == "asset-reference-committed";
    const bool filePublished = audit || (control.scenario != "asset-before-write" && control.scenario != "asset-before-rename");
    const auto reference = expectedAsset();
    verifyAssetFiles(control, filePublished);
    verify(kernel, audit ? Expected{4U, 2U, 2U, true} : committed ? Expected{2U, 2U, 2U, true}
        : Expected{1U, 1U, 1U, false}, {reference});
    check(handler.calls == 0U, "Bootstrap republished an asset");
    auto observer = take(SqlitePersistenceBackend::open({control.root / "state.db"}));
    const auto claims = take(observer->query("SELECT status FROM command_idempotency"));
    check(claims.size() == 1U && claims.front().at("status") == Value{audit || committed ? "completed" : "abandoned"},
        "Asset command idempotency disposition drift");
    const auto result = take(kernel.execution().executeCommand(createRequest()));
    check(result.replayed == (audit || committed) && result.commit && result.commit->changes.size() == 1U
        && result.commit->changes.front().after && result.commit->changes.front().after->assets == std::vector{reference},
        "Asset reference was lost from the command receipt");
    if(!audit) {
        verify(kernel, {2U, 2U, 2U, true}, {reference});
        take(kernel.execution().executeCommand(historyRequest(true)));
        verify(kernel, {3U, 1U, 2U, false}, {reference});
        take(kernel.execution().executeCommand(historyRequest(false)));
    }
    const auto replay = take(kernel.execution().executeCommand(createRequest()));
    check(replay.replayed && handler.calls == (!audit && !committed ? 1U : 0U), "Recovery repeated asset publication");
    verify(kernel, {4U, 2U, 2U, true}, {reference});
    verifyAssetFiles(control, true);
}

} // namespace

int runKernelCrashContract(std::string_view mode, const std::filesystem::path& root, std::string_view scenario)
{
    const std::array<std::string_view, 27> scenarios{"command-staged", "journal-inserted", "outcome-written",
        "transaction-before-commit", "transaction-committed", "command-returned",
        "undo-inserted", "undo-committed", "redo-inserted", "redo-committed", "task-handler",
        "workflow-handler", "workflow-committed", "effect-safe", "effect-idempotent", "effect-reconcile", "effect-never",
        "workflow-effect-reconcile", "workflow-effect-never", "asset-before-write", "asset-before-rename", "asset-after-rename",
        "asset-published", "asset-reference-inserted", "asset-reference-committed", "asset-missing-committed", "asset-corrupt-committed"};
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
    CrashControl control{std::string(scenario), false, {}, root};
    AppKernel kernel;
    auto startup = Result<void>::success();
    const bool expectedRejection = !seed && damagedAssetScenario(scenario);
    const auto handler = configure(kernel, root, control, seed, expectedRejection ? &startup : nullptr);
    if(expectedRejection) {
        check(!startup && containsError(startup.error(), "Asset.StateAdmissionFailed")
            && containsError(startup.error(), scenario == "asset-missing-committed" ? "Asset.NotFound" : "Asset.DigestMismatch"),
            "Recovery accepted a dangling or corrupted asset");
        check(kernel.state() == AppKernelState::Failed && !kernel.documents().snapshot(document)
            && handler->calls == 0U, "Failed asset recovery partially installed or executed state");
        check(take(kernel.persistence().journalAfter(document, 0U)).size() == 2U, "Recovery erased committed journal evidence");
        const auto candidate = take(kernel.persistence().recover());
        check(candidate.documents.size() == 1U && candidate.historyCommits.size() == 2U,
            "Failed admission erased persisted recovery/history materials");
        const auto& image = candidate.documents.front();
        const auto target = std::find_if(image.objects.begin(), image.objects.end(), [](const auto& object) {
            return object.id == id<ObjectId>("object.target");
        });
        check(image.documentId == document && image.projectId == project
            && image.revisions.at(RevisionScope::Document) == Revision{2U}
            && target != image.objects.end() && target->assets == std::vector{expectedAsset()},
            "Failed asset admission silently changed durable references");
        auto observer = take(SqlitePersistenceBackend::open({root / "state.db"}));
        const auto claims = take(observer->query("SELECT status FROM command_idempotency"));
        check(claims.size() == 1U && claims.front().at("status") == Value{"completed"},
            "Failed asset admission erased a committed receipt");
        check(std::filesystem::is_regular_file(root / "asset-original.snapshot"), "Original asset evidence missing");
        std::cout << (mode == "crash-audit" ? "crash-audited:" : "crash-recovered:") << scenario << '\n';
        return 0;
    }
    if(seed) {
        take(kernel.execution().executeCommand(createRequest(true)));
        take(kernel.documentRuntime().close(document));
        take(kernel.documentRuntime().open(document));
        if(scenario.starts_with("undo-") || scenario.starts_with("redo-")) {
            take(kernel.execution().executeCommand(createRequest()));
        }
        if(scenario.starts_with("redo-")) { take(kernel.execution().executeCommand(historyRequest(true, "seed"))); }
        if(scenario != "command-returned") {
            const auto before = take(kernel.documents().snapshot(document));
            control.verifyBeforeExit = [&kernel, &control, before] {
                const auto current = take(kernel.documents().snapshot(document));
                check(current.revisions() == before.revisions() && current.objects().all() == before.objects().all(),
                    "The document changed before the selected crash boundary");
                if(control.scenario == "task-handler") {
                    const auto running = take(kernel.execution().task(id<TaskId>("task.crash.instance")));
                    check(running.state == TaskState::Running && running.progress == 0.5
                        && kernel.scheduler().activeTaskCount() == 1U, "Task did not reach active execution");
                }
                if(damagedAssetScenario(control.scenario)) { damagePublishedAsset(control); }
            };
        }
        control.armed = true;
        if(executionScenario(scenario)) {
            if(scenario.starts_with("workflow-")) {
                const auto request = workflowRequest();
                take(kernel.execution().startWorkflow(request));
                take(kernel.execution().advanceWorkflow(request.workflowId));
            } else {
                take(kernel.execution().executeCommand(executionRequest(scenario == "task-handler")));
                if(scenario == "task-handler") {
                    take(kernel.execution().waitTask(id<TaskId>("task.crash.instance"), std::chrono::seconds{5}));
                }
            }
            throw std::runtime_error("Execution crash point was not reached");
        }
        take(kernel.execution().executeCommand(scenario.starts_with("undo-") ? historyRequest(true, "crash")
            : scenario.starts_with("redo-") ? historyRequest(false, "crash") : createRequest()));
        if(scenario == "command-returned") { verify(kernel, {2U, 2U, 2U, true}); control.terminate("command-returned"); }
        throw std::runtime_error("Crash point was not reached");
    }
    if(scenario.starts_with("asset-")) {
        recoverAsset(kernel, control, *handler, mode == "crash-audit");
        take(kernel.shutdown());
        std::cout << (mode == "crash-audit" ? "crash-audited:" : "crash-recovered:") << scenario << '\n';
        return 0;
    }
    if(executionScenario(scenario)) {
        recoverExecution(kernel, control, *handler, mode == "crash-audit");
        take(kernel.shutdown());
        std::cout << (mode == "crash-audit" ? "crash-audited:" : "crash-recovered:") << scenario << '\n';
        return 0;
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
