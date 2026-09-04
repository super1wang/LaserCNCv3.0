#include "kernel_test_module.hpp"
#include "fault_injecting_backend.hpp"
#include "persistence_fixture.hpp"
#include <lasercnc/infrastructure/bs_thread_pool_executor.hpp>
#include <lasercnc/observability/log_service.hpp>
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace lasercnc;
using namespace foundation;
using namespace kernel;
using namespace runtime;
using namespace state;
using namespace persistence;
using namespace infrastructure;
using namespace test;
using namespace std::chrono_literals;

template<typename T> T take(Result<T> result)
{
    if(!result) { throw std::runtime_error(std::string(result.error().code.value()) + ": " + result.error().message); }
    return std::move(result).value();
}
void take(Result<void> result)
{
    if(!result) { throw std::runtime_error(std::string(result.error().code.value()) + ": " + result.error().message); }
}
template<typename Id> Id id(std::string value) { return take(Id::create(std::move(value))); }
const auto project = id<ProjectId>("project.recovery-admission");
const auto document = id<DocumentId>("document.recovery-admission");
const auto session = id<SessionId>("session.recovery-admission");
const auto capability = id<CapabilityId>("recovery-admission.execute");
const auto correlation = id<CorrelationId>("correlation.recovery-admission");
const auto trace = id<TraceId>("trace.recovery-admission");

std::filesystem::path freshRoot()
{
    static std::atomic_uint sequence{0U};
    auto root = std::filesystem::path{LCNC_STRESS_TEST_ROOT} / "document-recovery-admission"
        / (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
            + "-" + std::to_string(sequence.fetch_add(1U)));
    REQUIRE(std::filesystem::create_directories(root));
    return root;
}
class NullLog final : public observability::ILogService {
public:
    Result<void> write(const observability::LogRecord&) override { return Result<void>::success(); }
    Result<void> flush() override { return Result<void>::success(); }
};
class References final : public IObjectReferenceEnumerator {
public:
    Result<std::vector<ObjectId>> enumerate(const Value& value) const override
    {
        const auto* data = value.getIf<Value::Object>();
        if(data && data->contains("ref")) {
            const auto* text = data->at("ref").getIf<std::string>();
            if(!text) { return Result<std::vector<ObjectId>>::failure(makeError(
                "Test.InvalidReference", ErrorCategory::Validation, "Expected a reference identity")); }
            auto target = ObjectId::create(*text);
            if(!target) { return Result<std::vector<ObjectId>>::failure(std::move(target).error()); }
            return Result<std::vector<ObjectId>>::success({std::move(target).value()});
        }
        return Result<std::vector<ObjectId>>::success({});
    }
};
class Validator final : public IObjectTypeValidator {
public:
    Result<void> validate(const Value&) const override
    {
        if(beforeValidation) { auto callback = std::exchange(beforeValidation, {}); callback(); }
        return Result<void>::success();
    }
    mutable std::function<void()> beforeValidation;
};
class Import final : public ICommandHandler {
public:
    Result<Value> execute(const CommandRequest& request, ApplicationTransaction& transaction) override
    {
        ++calls;
        const auto batch = *request.arguments.getIf<Value::Object>()->at("batch").getIf<std::string>();
        for(unsigned int index = 0U; index < 2U; ++index) {
            Value::Object data{{"batch", Value{batch}}};
            if(index == 1U && reference) { data.emplace("ref", Value{std::string(reference->value())}); }
            auto created = transaction.createObject({id<ObjectId>(batch + "." + std::to_string(index)),
                id<ObjectTypeId>("type.recovery-admission"), Value{std::move(data)}});
            if(!created) { return Result<Value>::failure(std::move(created).error()); }
        }
        auto collected = transaction.collectEvent({id<EventName>("event.recovery-admission.imported"),
            {1U, 0U, 0U}, std::nullopt, Value{batch}});
        if(!collected) { return Result<Value>::failure(std::move(collected).error()); }
        ++stagedEvents;
        return Result<Value>::success(Value{Value::Object{}});
    }
    unsigned int calls{0U};
    unsigned int stagedEvents{0U};
    std::optional<ObjectId> reference;
};
class Read final : public IQueryHandler {
public:
    Result<Value> execute(const QueryRequest&, const QueryContext& context) override
    {
        ++calls;
        return Result<Value>::success(Value{Value::Object{{"count",
            Value{static_cast<std::int64_t>(context.document->objects().size())}}}});
    }
    unsigned int calls{0U};
};
class Work final : public ITaskHandler {
public:
    Result<Value> execute(const TaskRequest&, const TaskContext&) override
    {
        ++calls;
        return Result<Value>::success(Value{Value::Object{}});
    }
    std::atomic_uint calls{0U};
};
class Plan final : public IAsyncCommandHandler {
public:
    Result<AsyncCommandPlan> prepare(const CommandRequest& request) override
    {
        ++calls;
        TaskRequest task{id<TaskId>("task." + std::string(request.requestId.value())),
            id<TaskName>("task.recovery-admission"), Value{Value::Object{}}, request.traceId,
            request.correlationId, request.context.projectId, request.context.documentId};
        return Result<AsyncCommandPlan>::success({std::move(task), Value{Value::Object{}}});
    }
    unsigned int calls{0U};
};
CommandRequest command(const std::string& suffix, const char* name = "test.recovery-admission.import")
{
    return {id<RequestId>("request." + suffix), {session, project, document}, id<CommandName>(name),
        {1U, 0U, 0U}, Value{Value::Object{{"batch", Value{suffix}}}}, std::nullopt, correlation, trace};
}
QueryRequest query(const std::string& suffix)
{
    return {id<RequestId>("query." + suffix), {session, project, document},
        id<QueryName>("query.recovery-admission"), {1U, 0U, 0U}, Value{Value::Object{}}, correlation, trace};
}
WorkflowRequest workflow(const std::string& suffix)
{
    return {id<WorkflowId>("workflow." + suffix), id<WorkflowName>("workflow.recovery-admission"),
        Value{Value::Object{}}, session, project, document, correlation, trace};
}
ScriptRequest script(const std::string& suffix)
{
    return {id<ScriptExecutionId>("script." + suffix), id<ScriptName>("script.recovery-admission"),
        Value{Value::Object{}}, session, project, document, correlation, trace};
}
template<typename T> void rejected(const Result<T>& result, const char* code)
{
    REQUIRE_FALSE(result);
    CHECK(std::string(result.error().code.value()) == code);
}

struct Fixture final {
    explicit Fixture(const std::filesystem::path& root)
    {
        take(kernel.executionServices().configure(std::make_shared<JsonconsAdapter>(), std::make_shared<NullLog>()));
        auto objectType = valueObjectType("type.recovery-admission");
        objectType.versions.front().validator = validator;
        objectType.versions.front().references = std::make_shared<References>();
        take(registerObjectType(kernel, std::move(objectType)));
        const auto schema = take(Schema::create(id<SchemaId>("schema.recovery-admission"), {1U, 0U, 0U}, SchemaKind::Object));
        take(registerCommand(kernel, CommandDescriptor{id<CommandName>("test.recovery-admission.import"),
            {1U, 0U, 0U}, schema, schema, ExecutionMode::Synchronous, SideEffectLevel::DocumentWrite,
            capability, true, true, false}, importer));
        take(registerQuery(kernel, QueryDescriptor{id<QueryName>("query.recovery-admission"),
            {1U, 0U, 0U}, schema, schema, capability, ExecutionScope::Document, true}, reader));
        take(kernel.configureTaskExecutor(take(BsThreadPoolExecutor::create({1U}))));
        take(registerTask(kernel, TaskDescriptor{id<TaskName>("task.recovery-admission"),
            {1U, 0U, 0U}, schema, schema}, worker));
        take(registerAsyncCommand(kernel, CommandDescriptor{id<CommandName>("test.recovery-admission.task"),
            {1U, 0U, 0U}, schema, schema, ExecutionMode::Asynchronous, SideEffectLevel::ReadOnly,
            capability, false, true, false}, planner));
        WorkflowStep step{id<WorkflowStepId>("step.read"), WorkflowStepKind::Query};
        step.query = WorkflowQueryCall{id<QueryName>("query.recovery-admission"), {1U, 0U, 0U}, Value{Value::Object{}}};
        take(registerWorkflow(kernel, WorkflowDefinition{{id<WorkflowName>("workflow.recovery-admission"),
            {1U, 0U, 0U}, schema, schema}, {step}, Value{Value::Object{}}}));
        ScriptNode node{id<ScriptNodeId>("node.read"), ScriptNodeKind::Query};
        node.query = ScriptQueryCall{id<QueryName>("query.recovery-admission"), {1U, 0U, 0U}, Value{Value::Object{}}, "read"};
        take(registerScript(kernel, ScriptDefinition{{id<ScriptName>("script.recovery-admission"),
            {1U, 0U, 0U}, schema, schema}, {node}, Value{Value::Object{}}}));
        take(kernel.capabilities().replace(session, std::array{capability, id<CapabilityId>("kernel.history.edit")}));
        auto backend = std::make_unique<FaultInjectingBackend>(take(SqlitePersistenceBackend::open({root / "state.db"})));
        faults = backend.get();
        take(kernel.configurePersistence(std::move(backend), std::make_shared<JsonconsAdapter>(),
            std::make_shared<Sha256HashService>(), take(FilesystemSnapshotStore::create({root / "snapshots", 1024U * 1024U}))));
        subscription.emplace(take(kernel.events().subscribe(id<SubscriptionId>("subscription.recovery-admission"),
            messaging::EventFilter{messaging::EventKind::Domain, std::nullopt}, messaging::DeliveryMode::Immediate,
            [this](const auto&) { ++events; })));
    }
    void seed()
    {
        take(kernel.bootstrap());
        take(kernel.projectRuntime().create(project));
        take(kernel.documentRuntime().create(project, document));
        const auto imported = take(kernel.execution().executeCommand(command("seed")));
        REQUIRE(imported.commit);
        CHECK(imported.commit->changes.size() == 2U);
        CHECK(imported.commit->events.size() == 1U);
        CHECK(imported.postExecutionErrors.empty());
        CHECK(events == 1U);
    }
    void healthyControls()
    {
        REQUIRE(kernel.execution().executeQuery(query("healthy")));
        const auto task = take(kernel.execution().executeCommand(command("healthy-task", "test.recovery-admission.task")));
        REQUIRE(task.taskId);
        CHECK(take(kernel.execution().waitTask(*task.taskId, 5s)).state == TaskState::Succeeded);
        REQUIRE(kernel.execution().startWorkflow(workflow("healthy")));
        CHECK(take(kernel.execution().advanceWorkflow(workflow("healthy").workflowId)).state == WorkflowState::Succeeded);
        REQUIRE(kernel.execution().executeScript(script("healthy")));
        CHECK(take(kernel.execution().advanceScript(script("healthy").executionId)).state == ScriptState::Succeeded);
        CHECK(reader->calls == 3U);
        CHECK(planner->calls == 1U);
        CHECK(worker->calls == 1U);
    }
    std::vector<platform::PersistenceRow> catalogRows()
    {
        return take(faults->query("SELECT * FROM document_catalog WHERE document_id=?", std::array{Value{std::string(document.value())}}));
    }
    void denyExecution(const std::string& suffix)
    {
        const std::array before{importer->calls, importer->stagedEvents, reader->calls, planner->calls, worker->calls.load(), events.load()};
        rejected(kernel.documentRuntime().snapshot(document), "Document.NotOpen");
        rejected(kernel.execution().executeCommand(command(suffix)), "Document.NotOpen");
        rejected(kernel.execution().executeQuery(query(suffix)), "Document.NotOpen");
        rejected(kernel.execution().executeCommand(command(suffix, "test.recovery-admission.task")), "Document.NotOpen");
        rejected(kernel.execution().startWorkflow(workflow(suffix)), "Document.NotOpen");
        rejected(kernel.execution().executeScript(script(suffix)), "Document.NotOpen");
        auto undo = command(suffix, "edit.undo");
        undo.arguments = Value{Value::Object{}};
        rejected(kernel.execution().executeCommand(undo), "Document.NotOpen");
        const std::array after{importer->calls, importer->stagedEvents, reader->calls, planner->calls, worker->calls.load(), events.load()};
        CHECK(after == before);
        CHECK_FALSE(kernel.execution().task(id<TaskId>("task.request." + suffix)));
        CHECK_FALSE(take(kernel.persistence().taskHistory(id<TaskId>("task.request." + suffix))).has_value());
        CHECK_FALSE(kernel.execution().workflow(workflow(suffix).workflowId));
        CHECK_FALSE(take(kernel.persistence().workflowCheckpoint(workflow(suffix).workflowId)).has_value());
        CHECK_FALSE(kernel.execution().script(script(suffix).executionId));
        CHECK(kernel.scheduler().activeTaskCount() == 0U);
        CHECK(take(kernel.projectRuntime().lifecycle(project)).activities == 0U);
        CHECK(take(kernel.documentRuntime().lifecycle(document)).activities == std::array<std::size_t, 6U>{});
    }
    std::atomic_uint events{0U};
    AppKernel kernel;
    std::optional<messaging::EventSubscription> subscription;
    std::shared_ptr<Import> importer{std::make_shared<Import>()};
    std::shared_ptr<Read> reader{std::make_shared<Read>()};
    std::shared_ptr<Work> worker{std::make_shared<Work>()};
    std::shared_ptr<Plan> planner{std::make_shared<Plan>()};
    std::shared_ptr<Validator> validator{std::make_shared<Validator>()};
    FaultInjectingBackend* faults{nullptr};
};
} // namespace

TEST_CASE("Recovery installation write failures cannot expose executable partial documents", "[document][recovery][admission][c4]")
{
    for(const auto point : {BackendPoint::Execute, BackendPoint::Commit}) {
        for(const unsigned int occurrence : {1U, 2U}) {
            for(const bool throws : {false, true}) {
                DYNAMIC_SECTION("point=" << static_cast<unsigned int>(point) << " occurrence=" << occurrence << " throws=" << throws) {
                    const auto root = freshRoot();
                    INFO("evidence=" << root.string());
                    std::vector<platform::PersistenceRow> failedRows;
                    std::optional<Document> original;
                    std::optional<TransactionId> transaction;
                    {
                        Fixture fixture{root};
                        auto& kernel = fixture.kernel;
                        fixture.seed();
                        fixture.healthyControls();
                        original = take(kernel.documents().snapshot(document));
                        transaction = take(kernel.history().snapshot(document)).entries.front().transactionId;
                        REQUIRE(kernel.documentRuntime().close(document));
                        const auto before = fixture.catalogRows();
                        fixture.faults->arm(point, "INSERT INTO document_catalog", occurrence, throws);
                        const auto opened = kernel.documentRuntime().open(document);
                        rejected(opened, throws ? "Persistence.DocumentCatalogWriteFailed" : "Test.BackendStageFailure");
                        CHECK(fixture.faults->hits == 1U);
                        CHECK(kernel.state() == AppKernelState::Ready);
                        const auto lifecycle = take(kernel.documentRuntime().lifecycle(document));
                        CHECK(lifecycle.state == DocumentLifecycleState::Failed);
                        REQUIRE(lifecycle.error);
                        CHECK(lifecycle.error->code == opened.error().code);
                        CHECK(kernel.documents().contains(document) == (occurrence == 2U));
                        if(occurrence == 2U) {
                            const auto image = take(kernel.documents().snapshot(document));
                            CHECK(image.objects().all() == original->objects().all());
                            CHECK(image.revisions() == original->revisions());
                        }
                        failedRows = fixture.catalogRows();
                        REQUIRE(failedRows.size() == 1U);
                        CHECK(failedRows.front().at("state") == Value{occurrence == 1U ? "detached" : "opening"});
                        if(occurrence == 1U) { CHECK(failedRows == before); }
                        fixture.denyExecution("failed");
                        CHECK_FALSE(kernel.documentRuntime().open(document));
                        CHECK_FALSE(kernel.documentRuntime().close(document));
                        CHECK_FALSE(kernel.documentRuntime().remove(document));
                        CHECK(fixture.catalogRows() == failedRows);
                        CHECK(take(kernel.persistence().journalAfter(document, 0U)).size() == 1U);
                        CHECK(take(kernel.history().snapshot(document)).cursor == HistoryCursor{1U, 1U});
                        CHECK(take(kernel.history().snapshot(document)).entries.front().transactionId == *transaction);
                        REQUIRE(kernel.shutdown());
                    }
                    for(unsigned int restart = 0U; restart < 2U; ++restart) {
                        Fixture recovered{root};
                        auto& kernel = recovered.kernel;
                        REQUIRE(kernel.bootstrap());
                        CHECK(kernel.state() == AppKernelState::Ready);
                        CHECK_FALSE(kernel.documents().contains(document));
                        CHECK(recovered.catalogRows() == failedRows);
                        const auto lifecycle = take(kernel.documentRuntime().lifecycle(document));
                        CHECK(lifecycle.state == (occurrence == 1U ? DocumentLifecycleState::Detached : DocumentLifecycleState::Failed));
                        recovered.denyExecution("restarted");
                        CHECK(recovered.events == 0U);
                        CHECK(take(kernel.history().snapshot(document)).entries.front().transactionId == *transaction);
                        if(occurrence == 1U) {
                            REQUIRE(kernel.documentRuntime().open(document));
                            const auto image = take(kernel.documents().snapshot(document));
                            CHECK(image.objects().all() == original->objects().all());
                            CHECK(image.revisions() == original->revisions());
                            REQUIRE(kernel.execution().executeQuery(query("reopened")));
                            REQUIRE(kernel.documentRuntime().close(document));
                            failedRows = recovered.catalogRows();
                        } else {
                            rejected(kernel.documentRuntime().open(document), "Document.NotDetached");
                            REQUIRE(lifecycle.error);
                            CHECK(std::string(lifecycle.error->code.value()) == "Document.RecoveryInterruptedTransition");
                            CHECK(recovered.catalogRows() == failedRows);
                        }
                        CHECK(take(kernel.persistence().journalAfter(document, 0U)).size() == 1U);
                        REQUIRE(kernel.shutdown());
                    }
                }
            }
        }
    }
}

TEST_CASE("Governed batch import persistence faults preserve state events and retry through the gateway", "[document][transaction][failure][c4]")
{
    for(const auto point : {BackendPoint::Begin, BackendPoint::Execute, BackendPoint::Commit}) {
        for(const bool throws : {false, true}) {
            DYNAMIC_SECTION("point=" << static_cast<unsigned int>(point) << " throws=" << throws) {
                const auto root = freshRoot();
                INFO("evidence=" << root.string());
                std::optional<Document> acceptedImage;
                {
                    Fixture fixture{root};
                    auto& kernel = fixture.kernel;
                    fixture.seed();
                    REQUIRE(kernel.documentRuntime().close(document));
                    REQUIRE(kernel.documentRuntime().open(document));
                    const auto original = take(kernel.documents().snapshot(document));
                    const auto history = take(kernel.history().snapshot(document));
                    const auto rows = fixture.catalogRows();
                    const auto request = command("candidate");
                    fixture.faults->arm(point, point == BackendPoint::Begin ? "" : "INSERT INTO state_journal", 1U, throws);
                    rejected(kernel.execution().executeCommand(request),
                        throws ? "Persistence.JournalAppendFailed" : "Test.BackendStageFailure");
                    CHECK(fixture.faults->hits == 1U);
                    CHECK(fixture.importer->calls == 2U);
                    CHECK(fixture.importer->stagedEvents == 2U);
                    CHECK(fixture.events == 1U);
                    const auto unchanged = take(kernel.documents().snapshot(document));
                    CHECK(unchanged.objects().all() == original.objects().all());
                    CHECK(unchanged.revisions() == original.revisions());
                    CHECK(fixture.catalogRows() == rows);
                    const auto journal = take(kernel.persistence().journalAfter(document, 0U));
                    REQUIRE(journal.size() == 1U);
                    CHECK(journal.front().transactionId == history.entries.front().transactionId);
                    CHECK(take(kernel.history().snapshot(document)).cursor == HistoryCursor{1U, 1U});
                    CHECK(take(kernel.history().snapshot(document)).entries.front().transactionId == history.entries.front().transactionId);
                    CHECK(take(kernel.documentRuntime().lifecycle(document)).state == DocumentLifecycleState::Open);
                    CHECK(take(kernel.documentRuntime().lifecycle(document)).activities == std::array<std::size_t, 6U>{});
                    CHECK(take(kernel.projectRuntime().lifecycle(project)).activities == 0U);
                    REQUIRE(kernel.execution().executeQuery(query("after-fault")));
                    const auto accepted = take(kernel.execution().executeCommand(request));
                    REQUIRE(accepted.commit);
                    CHECK(accepted.commit->changes.size() == 2U);
                    CHECK(accepted.commit->events.size() == 1U);
                    CHECK(accepted.postExecutionErrors.empty());
                    CHECK(fixture.importer->calls == 3U);
                    CHECK(fixture.importer->stagedEvents == 3U);
                    CHECK(fixture.events == 2U);
                    acceptedImage = take(kernel.documents().snapshot(document));
                    CHECK(acceptedImage->objects().size() == 4U);
                    CHECK(acceptedImage->revisions().at(RevisionScope::Project) == Revision{2U});
                    CHECK(acceptedImage->revisions().at(RevisionScope::Document) == Revision{2U});
                    CHECK(take(kernel.persistence().journalAfter(document, 0U)).size() == 2U);
                    CHECK(take(kernel.history().snapshot(document)).cursor == HistoryCursor{2U, 2U});
                    REQUIRE(kernel.documentRuntime().close(document));
                    REQUIRE(kernel.shutdown());
                }
                Fixture recovered{root};
                auto& kernel = recovered.kernel;
                REQUIRE(kernel.bootstrap());
                CHECK(recovered.importer->calls == 0U);
                CHECK(recovered.events == 0U);
                REQUIRE(kernel.documentRuntime().open(document));
                const auto image = take(kernel.documents().snapshot(document));
                CHECK(image.objects().all() == acceptedImage->objects().all());
                CHECK(image.revisions() == acceptedImage->revisions());
                auto undo = command("undo", "edit.undo");
                undo.arguments = Value{Value::Object{}};
                REQUIRE(kernel.execution().executeCommand(undo));
                CHECK(take(kernel.documents().snapshot(document)).objects().size() == 2U);
                CHECK(take(kernel.history().snapshot(document)).cursor == HistoryCursor{1U, 2U});
                undo.requestId = id<RequestId>("request.redo");
                undo.command = id<CommandName>("edit.redo");
                REQUIRE(kernel.execution().executeCommand(undo));
                CHECK(take(kernel.documents().snapshot(document)).objects().all() == acceptedImage->objects().all());
                CHECK(take(kernel.history().snapshot(document)).cursor == HistoryCursor{2U, 2U});
                REQUIRE(kernel.shutdown());
            }
        }
    }
}

TEST_CASE("Bootstrap catalog installation failures retain diagnostics but never open execution admission", "[kernel][recovery][admission][c4]")
{
    for(const auto point : {BackendPoint::Execute, BackendPoint::Commit}) {
        for(const bool throws : {false, true}) {
            DYNAMIC_SECTION("point=" << static_cast<unsigned int>(point) << " throws=" << throws) {
                const auto root = freshRoot();
                INFO("evidence=" << root.string());
                const auto added = id<DocumentId>("document.startup-added");
                std::optional<Document> original;
                std::vector<platform::PersistenceRow> rows;
                {
                    Fixture seed{root};
                    seed.seed();
                    seed.healthyControls();
                    original = take(seed.kernel.documents().snapshot(document));
                    rows = seed.catalogRows();
                    REQUIRE(seed.kernel.shutdown());
                }
                {
                    Fixture failed{root};
                    auto& kernel = failed.kernel;
                    REQUIRE(kernel.addDocument(project, added));
                    failed.faults->arm(point, "INSERT INTO document_catalog", 1U, throws);
                    rejected(kernel.bootstrap(), "Persistence.KernelDocumentCatalogSyncFailed");
                    CHECK(failed.faults->hits == 1U);
                    CHECK(kernel.state() == AppKernelState::Failed);
                    REQUIRE(kernel.documents().contains(document));
                    REQUIRE(kernel.documents().contains(added));
                    const auto retained = take(kernel.documents().snapshot(document));
                    CHECK(retained.objects().all() == original->objects().all());
                    CHECK(retained.revisions() == original->revisions());
                    CHECK(failed.catalogRows() == rows);
                    CHECK(take(kernel.persistence().documentCatalog()).size() == 1U);
                    rejected(kernel.documentRuntime().snapshot(document), "Document.RuntimeNotAccepting");
                    rejected(kernel.execution().executeCommand(command("bootstrap-denied")), "Command.RuntimeNotReady");
                    rejected(kernel.execution().executeQuery(query("bootstrap-denied")), "Query.RuntimeNotReady");
                    rejected(kernel.execution().executeCommand(command("bootstrap-denied", "test.recovery-admission.task")), "Command.RuntimeNotReady");
                    rejected(kernel.execution().startWorkflow(workflow("bootstrap-denied")), "Workflow.RuntimeNotAccepting");
                    rejected(kernel.execution().executeScript(script("bootstrap-denied")), "Script.RuntimeNotAccepting");
                    CHECK_FALSE(kernel.documentRuntime().open(document));
                    CHECK_FALSE(kernel.documentRuntime().create(project, id<DocumentId>("document.bootstrap-denied")));
                    CHECK_FALSE(kernel.projectRuntime().create(id<ProjectId>("project.bootstrap-denied")));
                    CHECK_FALSE(kernel.bootstrap());
                    CHECK(failed.importer->calls == 0U);
                    CHECK(failed.reader->calls == 0U);
                    CHECK(failed.planner->calls == 0U);
                    CHECK(failed.worker->calls == 0U);
                    CHECK(failed.events == 0U);
                    CHECK(kernel.scheduler().activeTaskCount() == 0U);
                    CHECK(take(kernel.persistence().journalAfter(document, 0U)).size() == 1U);
                    CHECK(take(kernel.history().snapshot(document)).cursor == HistoryCursor{1U, 1U});
                    CHECK(failed.catalogRows() == rows);
                    rejected(kernel.shutdown(), "Kernel.AppKernelCannotStop");
                    CHECK(kernel.state() == AppKernelState::Failed);
                }
                Fixture recovered{root};
                auto& kernel = recovered.kernel;
                REQUIRE(kernel.bootstrap());
                CHECK(kernel.state() == AppKernelState::Ready);
                CHECK_FALSE(kernel.documents().contains(added));
                CHECK_FALSE(kernel.documentRuntime().lifecycle(added));
                const auto image = take(kernel.documents().snapshot(document));
                CHECK(image.objects().all() == original->objects().all());
                CHECK(image.revisions() == original->revisions());
                REQUIRE(kernel.execution().executeQuery(query("bootstrap-recovered")));
                CHECK(recovered.importer->calls == 0U);
                CHECK(recovered.planner->calls == 0U);
                CHECK(recovered.worker->calls == 0U);
                CHECK(recovered.events == 0U);
                CHECK(recovered.catalogRows() == rows);
                REQUIRE(kernel.shutdown());
            }
        }
    }
}

TEST_CASE("Governed batch import rejects unresolved and foreign document references without publishing its prefix", "[document][object-type][failure][c4]")
{
    for(const bool foreign : {false, true}) {
        DYNAMIC_SECTION("foreign=" << foreign) {
            const auto root = freshRoot();
            INFO("evidence=" << root.string());
            Fixture fixture{root};
            auto& kernel = fixture.kernel;
            fixture.seed();
            const auto other = id<DocumentId>("document.foreign-reference");
            if(foreign) {
                REQUIRE(kernel.documentRuntime().create(project, other));
                auto seedOther = command("foreign");
                seedOther.context.documentId = other;
                REQUIRE(kernel.execution().executeCommand(seedOther));
                CHECK(take(kernel.documents().snapshot(other)).objects().find(id<ObjectId>("foreign.0")) != nullptr);
            }
            const auto original = take(kernel.documents().snapshot(document));
            const auto delivered = fixture.events.load();
            const auto staged = fixture.importer->stagedEvents;
            fixture.importer->reference = id<ObjectId>(foreign ? "foreign.0" : "object.missing");
            const auto request = command("candidate");
            rejected(kernel.execution().executeCommand(request), "ObjectType.DanglingReference");
            CHECK(fixture.importer->stagedEvents == staged + 1U);
            CHECK(fixture.events == delivered);
            CHECK(take(kernel.documents().snapshot(document)).objects().all() == original.objects().all());
            CHECK(take(kernel.documents().snapshot(document)).revisions() == original.revisions());
            CHECK(take(kernel.persistence().journalAfter(document, 0U)).size() == 1U);
            CHECK(take(kernel.history().snapshot(document)).cursor == HistoryCursor{1U, 1U});
            CHECK(take(kernel.documentRuntime().lifecycle(document)).activities == std::array<std::size_t, 6U>{});
            if(foreign) {
                CHECK(take(kernel.documents().snapshot(other)).objects().size() == 2U);
                CHECK(take(kernel.persistence().journalAfter(other, 0U)).size() == 1U);
                CHECK(take(kernel.history().snapshot(other)).cursor == HistoryCursor{1U, 1U});
            }
            fixture.importer->reference.reset();
            REQUIRE(kernel.execution().executeCommand(request));
            CHECK(fixture.events == delivered + 1U);
            CHECK(take(kernel.documents().snapshot(document)).objects().size() == 4U);
            CHECK(take(kernel.history().snapshot(document)).cursor == HistoryCursor{2U, 2U});
            REQUIRE(kernel.shutdown());
        }
    }
}

TEST_CASE("Recovery installation reentry preserves a removed identity or a fully opened winner", "[document][recovery][reentry][c4]")
{
    for(const bool remove : {true, false}) {
        DYNAMIC_SECTION("remove=" << remove) {
            const auto root = freshRoot();
            INFO("evidence=" << root.string());
            std::optional<Document> original;
            std::vector<platform::PersistenceRow> winnerRows;
            {
                Fixture fixture{root};
                auto& kernel = fixture.kernel;
                fixture.seed();
                original = take(kernel.documents().snapshot(document));
                REQUIRE(kernel.documentRuntime().close(document));
                unsigned int callbacks = 0U;
                unsigned int winnerWrites = 0U;
                fixture.validator->beforeValidation = [&] {
                    ++callbacks;
                    const auto before = fixture.faults->beginCalls;
                    rejected(kernel.shutdown(), "Kernel.ActiveExecutions");
                    rejected(kernel.projectRuntime().close(project), "Project.CloseBlocked");
                    CHECK(fixture.faults->beginCalls == before);
                    if(remove) { REQUIRE(kernel.documentRuntime().remove(document)); }
                    else { REQUIRE(kernel.documentRuntime().open(document)); }
                    winnerRows = fixture.catalogRows();
                    winnerWrites = fixture.faults->beginCalls;
                };
                rejected(kernel.documentRuntime().open(document), remove ? "Document.LifecycleNotFound" : "Document.LifecycleConflict");
                CHECK(callbacks == 1U);
                CHECK(fixture.faults->beginCalls == winnerWrites);
                CHECK(fixture.catalogRows() == winnerRows);
                REQUIRE(winnerRows.size() == 1U);
                CHECK(winnerRows.front().at("state") == Value{remove ? "removed" : "open"});
                CHECK(kernel.documents().contains(document) == !remove);
                if(remove) {
                    CHECK_FALSE(kernel.documentRuntime().lifecycle(document));
                    rejected(kernel.documentRuntime().open(document), "Document.NotDetached");
                    CHECK_FALSE(kernel.execution().executeQuery(query("removed")));
                } else {
                    CHECK(take(kernel.documentRuntime().lifecycle(document)).state == DocumentLifecycleState::Open);
                    const auto image = take(kernel.documents().snapshot(document));
                    CHECK(image.objects().all() == original->objects().all());
                    CHECK(image.revisions() == original->revisions());
                    REQUIRE(kernel.execution().executeQuery(query("winner")));
                }
                CHECK(take(kernel.persistence().journalAfter(document, 0U)).size() == 1U);
                CHECK(take(kernel.history().snapshot(document)).cursor == HistoryCursor{1U, 1U});
                CHECK(fixture.events == 1U);
                REQUIRE(kernel.shutdown());
            }
            Fixture recovered{root};
            auto& kernel = recovered.kernel;
            REQUIRE(kernel.bootstrap());
            CHECK(recovered.catalogRows() == winnerRows);
            CHECK(kernel.documents().contains(document) == !remove);
            CHECK(recovered.events == 0U);
            CHECK(recovered.importer->calls == 0U);
            if(remove) {
                CHECK_FALSE(kernel.documentRuntime().lifecycle(document));
                CHECK_FALSE(kernel.documentRuntime().create(project, document));
                rejected(kernel.documentRuntime().open(document), "Document.NotDetached");
            } else {
                const auto image = take(kernel.documents().snapshot(document));
                CHECK(image.objects().all() == original->objects().all());
                CHECK(image.revisions() == original->revisions());
                REQUIRE(kernel.execution().executeQuery(query("recovered-winner")));
            }
            REQUIRE(kernel.shutdown());
        }
    }
}
