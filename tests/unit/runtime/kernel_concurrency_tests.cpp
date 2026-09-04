#include "kernel_test_module.hpp"
#include "persistence_fixture.hpp"

#include <lasercnc/infrastructure/filesystem_snapshot_store.hpp>
#include <lasercnc/infrastructure/bs_thread_pool_executor.hpp>
#include <lasercnc/infrastructure/jsoncons_adapter.hpp>
#include <lasercnc/infrastructure/sha256_hash_service.hpp>
#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace lasercnc;
using namespace foundation;
using namespace kernel;
using namespace runtime;
using namespace state;
using namespace infrastructure;
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
template<typename Id> Id id(const char* value) { return take(Id::create(value)); }
const auto project = id<ProjectId>("project.stress");
const auto document = id<DocumentId>("document.stress");
const auto session = id<SessionId>("session.stress");
constexpr unsigned int rounds = 20U;

class TimedGate final {
public:
    explicit TimedGate(std::chrono::milliseconds timeout = 5s) : timeout_(timeout) {}

    void arriveAndWait()
    {
        std::unique_lock lock(mutex_);
        ++arrivals_;
        changed_.notify_all();
        if(!changed_.wait_for(lock, timeout_, [this] { return released_; })) {
            throw std::runtime_error("Test.GateTimeout: worker release");
        }
    }
    bool awaitArrivals(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout_, [this, count] { return arrivals_ >= count; });
    }
    void release()
    {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }
private:
    std::chrono::milliseconds timeout_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t arrivals_{0U};
    bool released_{false};
};

// Declare after futures: a failed assertion must release workers before joining them.
// 中文翻译：在 future 之后声明，断言失败时先释放工作线程，再等待其退出。
struct ReleaseOnExit final {
    TimedGate& gate;
    ~ReleaseOnExit() { gate.release(); }
};

template<typename T> T completed(std::future<T>& future, std::chrono::milliseconds timeout = 5s)
{
    if(future.wait_for(timeout) != std::future_status::ready) {
        throw std::runtime_error("Test.FutureTimeout: execution did not finish");
    }
    return future.get();
}
template<typename Predicate> bool awaitCondition(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        if(predicate()) { return true; }
        std::this_thread::yield();
    } while(std::chrono::steady_clock::now() < deadline);
    return false;
}

class NullLog final : public observability::ILogService {
public:
    Result<void> write(const observability::LogRecord&) override { return Result<void>::success(); }
    Result<void> flush() override { return Result<void>::success(); }
};

class CreateHandler final : public ICommandHandler {
public:
    Result<Value> execute(const CommandRequest& request, ApplicationTransaction& transaction) override
    {
        ++calls;
        const auto name = *request.arguments.getIf<Value::Object>()->at("id").getIf<std::string>();
        take(transaction.createObject({take(ObjectId::create(name)), id<ObjectTypeId>("type.stress"), Value{name}}));
        take(transaction.touchRevision(RevisionScope::Geometry));
        if(gate) { gate->arriveAndWait(); }
        return Result<Value>::success(Value{Value::Object{{"id", Value{name}}}});
    }
    std::atomic_uint calls{0U};
    std::shared_ptr<TimedGate> gate;
};

class QueryHandler final : public IQueryHandler {
public:
    Result<Value> execute(const QueryRequest&, const QueryContext& context) override
    {
        ++calls;
        const auto image = context.document.value();
        if(gate) { gate->arriveAndWait(); }
        return Result<Value>::success(Value{Value::Object{
            {"objects", Value{static_cast<std::int64_t>(image.objects().size())}},
            {"revision", Value{std::to_string(image.revisions().at(RevisionScope::Document).value())}}}});
    }
    std::atomic_uint calls{0U};
    std::shared_ptr<TimedGate> gate;
};

class GatedSnapshotStore final : public platform::ISnapshotStore {
public:
    explicit GatedSnapshotStore(std::unique_ptr<platform::ISnapshotStore> store) : store_(std::move(store)) {}
    Result<platform::SnapshotWriteDisposition> writeAtomically(const SnapshotId& key, std::string_view payload) override
    {
        if(armed.load()) { gate.arriveAndWait(); }
        return store_->writeAtomically(key, payload);
    }
    Result<std::string> read(const SnapshotId& key) const override { return store_->read(key); }
    Result<bool> remove(const SnapshotId& key) override { return store_->remove(key); }
    TimedGate gate;
    std::atomic_bool armed{false};
private:
    std::unique_ptr<platform::ISnapshotStore> store_;
};

const auto taskResource = id<ResourceId>("resource.stress.task");
class GatedTaskHandler final : public ITaskHandler {
public:
    Result<Value> execute(const TaskRequest&, const TaskContext& context) override
    {
        ++calls;
        if(std::this_thread::get_id() == caller || !context.document
           || context.document->id() != document || context.resources.claims.size() != 1U) {
            throw std::runtime_error("Test.InvalidTaskContext");
        }
        take(context.progress.report(0.5, "held at completion boundary"));
        gate.arriveAndWait();
        return Result<Value>::success(Value{Value::Object{{"done", Value{true}}}});
    }
    TimedGate gate;
    std::atomic_uint calls{0U};
    std::thread::id caller{std::this_thread::get_id()};
};

class TaskPlanHandler final : public IAsyncCommandHandler {
public:
    Result<AsyncCommandPlan> prepare(const CommandRequest& request) override
    {
        TaskRequest task{take(TaskId::create("task." + std::string(request.requestId.value()))),
            id<TaskName>("task.stress"), Value{Value::Object{}}, request.traceId, request.correlationId,
            project, document};
        task.resources = {{ResourceKind::DiskIO, taskResource, ResourceAccess::Exclusive, 1U}};
        return Result<AsyncCommandPlan>::success({std::move(task), Value{Value::Object{{"accepted", Value{true}}}}});
    }
};

enum class ExecutionScenario { None, Task, Workflow };

std::filesystem::path newRoot()
{
    static std::atomic_ullong sequence{0U};
    const auto base = std::filesystem::path{LCNC_STRESS_TEST_ROOT};
    std::filesystem::create_directories(base);
    const auto root = base / (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
        + '-' + std::to_string(sequence.fetch_add(1U)));
    if(!std::filesystem::create_directory(root)) { throw std::runtime_error("Stress evidence directory already exists"); }
    return root;
}

struct Fixture final {
    explicit Fixture(const std::filesystem::path& root, bool seed = true,
        ExecutionScenario execution = ExecutionScenario::None)
    {
        if(seed) { take(kernel.addDocument(project, document)); }
        take(kernel.executionServices().configure(std::make_shared<JsonconsAdapter>(), std::make_shared<NullLog>()));
        take(test::registerObjectType(kernel, test::valueObjectType("type.stress")));
        auto schema = take(Schema::create(id<SchemaId>("schema.stress"), {1U, 0U, 0U}, SchemaKind::Object));
        take(test::registerCommand(kernel, CommandDescriptor{id<CommandName>("test.stress.create"), {1U, 0U, 0U},
            schema, schema, ExecutionMode::Synchronous, SideEffectLevel::DocumentWrite,
            id<CapabilityId>("document.write"), true, true, true}, command));
        take(test::registerQuery(kernel, QueryDescriptor{id<QueryName>("test.stress.read"), {1U, 0U, 0U},
            schema, schema, id<CapabilityId>("document.read"), ExecutionScope::Document, true}, query));
        if(execution == ExecutionScenario::Task) {
            take(kernel.configureTaskExecutor(take(BsThreadPoolExecutor::create({1U}))));
            take(kernel.resources().configure(ResourceKind::DiskIO, taskResource, 1U));
            take(test::registerTask(kernel, TaskDescriptor{id<TaskName>("task.stress"), {1U, 0U, 0U}, schema, schema}, task));
            take(test::registerAsyncCommand(kernel, CommandDescriptor{id<CommandName>("test.stress.task"), {1U, 0U, 0U},
                schema, schema, ExecutionMode::Asynchronous, SideEffectLevel::ReadOnly,
                id<CapabilityId>("document.read"), false, true, true}, std::make_shared<TaskPlanHandler>()));
        }
        if(execution == ExecutionScenario::Workflow) {
            WorkflowStep first{id<WorkflowStepId>("step.stress.first"), WorkflowStepKind::Command};
            first.command = WorkflowCommandCall{id<CommandName>("test.stress.create"), {1U, 0U, 0U},
                Value{Value::Object{{"id", Value{"object.stress.first"}}}}};
            WorkflowStep second{id<WorkflowStepId>("step.stress.second"), WorkflowStepKind::Command};
            second.dependencies = {first.stepId};
            second.command = WorkflowCommandCall{id<CommandName>("test.stress.create"), {1U, 0U, 0U},
                Value{Value::Object{{"id", Value{"object.stress.second"}}}}};
            take(test::registerWorkflow(kernel, WorkflowDefinition{WorkflowDescriptor{
                id<WorkflowName>("workflow.stress"), {1U, 0U, 0U}, schema, schema},
                {first, second}, Value{Value::Object{}}}));
        }
        take(kernel.capabilities().replace(session, std::array{id<CapabilityId>("document.write"), id<CapabilityId>("document.read")}));
        auto snapshotStore = std::make_unique<GatedSnapshotStore>(take(FilesystemSnapshotStore::create({root / "snapshots", 1024U * 1024U})));
        store = snapshotStore.get();
        take(kernel.configurePersistence(take(SqlitePersistenceBackend::open({root / "state.db"})),
            std::make_shared<JsonconsAdapter>(), std::make_shared<Sha256HashService>(), std::move(snapshotStore)));
        take(kernel.bootstrap());
        if(seed) {
            // Seed through the owning Kernel's normal close/open path.
            // 中文翻译：由持有所有权的 Kernel 经正常关闭/打开建立持久基线。
            take(kernel.documentRuntime().close(document));
            take(kernel.documentRuntime().open(document));
        }
    }
    AppKernel kernel;
    std::shared_ptr<CreateHandler> command{std::make_shared<CreateHandler>()};
    std::shared_ptr<QueryHandler> query{std::make_shared<QueryHandler>()};
    std::shared_ptr<GatedTaskHandler> task{std::make_shared<GatedTaskHandler>()};
    GatedSnapshotStore* store{nullptr};
};

CommandRequest createRequest(unsigned int index = 0U)
{
    const auto suffix = std::to_string(index);
    return {take(RequestId::create("request.stress." + suffix)), {session, project, document},
        id<CommandName>("test.stress.create"), {1U, 0U, 0U},
        Value{Value::Object{{"id", Value{"object.stress." + suffix}}}}, Revision{},
        id<CorrelationId>("correlation.stress"), id<TraceId>("trace.stress")};
}
QueryRequest queryRequest()
{
    return {id<RequestId>("request.stress.query"), {session, project, document}, id<QueryName>("test.stress.read"),
        {1U, 0U, 0U}, Value{Value::Object{}}, id<CorrelationId>("correlation.stress"), id<TraceId>("trace.stress")};
}
std::size_t active(AppKernel& kernel, DocumentActivityKind kind)
{
    return take(kernel.documentRuntime().lifecycle(document)).activities.at(static_cast<std::size_t>(kind));
}
void verifyState(AppKernel& kernel, std::size_t count)
{
    const auto image = take(kernel.documents().snapshot(document));
    REQUIRE(image.objects().size() == count);
    for(const auto& object : image.objects().all()) {
        REQUIRE(object.type == id<ObjectTypeId>("type.stress"));
        REQUIRE(object.data == Value{std::string(object.id.value())});
        REQUIRE(object.assets.empty());
    }
    for(const auto scope : {RevisionScope::Project, RevisionScope::Document, RevisionScope::Geometry}) {
        REQUIRE(image.revisions().at(scope) == Revision{count});
    }
    for(const auto scope : {RevisionScope::Cam, RevisionScope::MachineContext, RevisionScope::Environment}) {
        REQUIRE(image.revisions().at(scope) == Revision{});
    }
    const auto history = take(kernel.history().snapshot(document));
    REQUIRE(history.cursor == HistoryCursor{count, count});
    REQUIRE(history.entries.size() == count);
    REQUIRE_FALSE(history.barrier.has_value());
    const auto journal = take(kernel.persistence().journalAfter(document, 0U));
    REQUIRE(journal.size() == count);
    for(std::size_t index = 0U; index < journal.size(); ++index) {
        REQUIRE(journal[index].revisionsBefore.at(RevisionScope::Document) == Revision{index});
        REQUIRE(journal[index].revisionsAfter.at(RevisionScope::Document) == Revision{index + 1U});
    }
    for(const auto countActive : take(kernel.documentRuntime().lifecycle(document)).activities) {
        REQUIRE(countActive == 0U);
    }
    REQUIRE(kernel.scheduler().activeTaskCount() == 0U);
}
void verifyQuery(const QueryResponse& response)
{
    REQUIRE(response.result == Value{Value::Object{{"objects", Value{std::int64_t{0}}}, {"revision", Value{"0"}}}});
    REQUIRE(response.revisions == RevisionSet{});
    REQUIRE(response.postExecutionErrors.empty());
}

enum class CancelOrder { BeforeCompletion, AfterCompletion, Race };
CommandRequest taskCommand()
{
    auto request = createRequest();
    request.command = id<CommandName>("test.stress.task");
    request.arguments = Value{Value::Object{}};
    request.idempotencyKey = id<IdempotencyKey>("key.stress.task");
    return request;
}
void verifyResources(AppKernel& kernel, bool held)
{
    const auto resources = kernel.resources().snapshot();
    REQUIRE(resources.size() == 1U);
    REQUIRE(resources.front().resource == taskResource);
    REQUIRE(resources.front().exclusivelyHeld == held);
    REQUIRE(resources.front().sharedUnits == 0U);
}
void verifyTask(const TaskSnapshot& snapshot, TaskState expected)
{
    REQUIRE(snapshot.state == expected);
    REQUIRE(snapshot.sourceRevisions == RevisionSet{});
    if(expected == TaskState::Cancelled) {
        REQUIRE(snapshot.error.has_value());
        REQUIRE(std::string(snapshot.error->code.value()) == "Task.Cancelled");
        REQUIRE_FALSE(snapshot.result.has_value());
    } else {
        REQUIRE_FALSE(snapshot.error.has_value());
        REQUIRE(snapshot.result == Value{Value::Object{{"done", Value{true}}}});
    }
}

void exerciseTaskCancellation(CancelOrder order)
{
    for(unsigned int round = 0U; round < rounds; ++round) {
        const auto root = newRoot();
        INFO("round=" << round << " evidence=" << root.string());
        const auto request = taskCommand();
        std::optional<TaskId> taskId;
        TaskState finalState{TaskState::Failed};
        {
            Fixture fixture{root, true, ExecutionScenario::Task};
            auto& kernel = fixture.kernel;
            TimedGate race;
            std::vector<std::future<Result<void>>> cancels;
            std::future<void> finish;
            ReleaseOnExit releaseWorker{fixture.task->gate};
            ReleaseOnExit releaseRace{race};
            const auto accepted = take(kernel.execution().executeCommand(request));
            REQUIRE(accepted.taskId.has_value());
            taskId = accepted.taskId;
            REQUIRE(fixture.task->gate.awaitArrivals(1U));
            REQUIRE(take(kernel.execution().task(*taskId)).state == TaskState::Running);
            REQUIRE(kernel.scheduler().activeTaskCount() == 1U);
            verifyResources(kernel, true);
            const auto closed = kernel.documentRuntime().close(document);
            REQUIRE_FALSE(closed);
            REQUIRE(std::string(closed.error().code.value()) == "Document.CloseBlocked");
            REQUIRE(take(kernel.documentRuntime().lifecycle(document)).state == DocumentLifecycleState::Open);
            if(order == CancelOrder::AfterCompletion) {
                fixture.task->gate.release();
                verifyTask(take(kernel.execution().waitTask(*taskId, 5s)), TaskState::Succeeded);
            }
            for(unsigned int index = 0U; index < 8U; ++index) {
                cancels.push_back(std::async(std::launch::async, [&] {
                    if(order == CancelOrder::Race) { race.arriveAndWait(); }
                    return kernel.execution().cancelTask(*taskId);
                }));
            }
            if(order == CancelOrder::Race) {
                finish = std::async(std::launch::async, [&] { race.arriveAndWait(); fixture.task->gate.release(); });
                REQUIRE(race.awaitArrivals(9U));
                race.release();
            }
            for(auto& future : cancels) { REQUIRE(completed(future)); }
            if(order == CancelOrder::BeforeCompletion) {
                REQUIRE(take(kernel.execution().task(*taskId)).state == TaskState::CancelRequested);
                REQUIRE(kernel.scheduler().activeTaskCount() == 1U);
                verifyResources(kernel, true);
                const auto stillBlocked = kernel.documentRuntime().close(document);
                REQUIRE_FALSE(stillBlocked);
                REQUIRE(std::string(stillBlocked.error().code.value()) == "Document.CloseBlocked");
                fixture.task->gate.release();
            }
            if(finish.valid()) { completed(finish); }
            const auto terminal = take(kernel.execution().waitTask(*taskId, 5s));
            finalState = terminal.state;
            if(order == CancelOrder::BeforeCompletion) { REQUIRE(finalState == TaskState::Cancelled); }
            else if(order == CancelOrder::AfterCompletion) { REQUIRE(finalState == TaskState::Succeeded); }
            else { REQUIRE((finalState == TaskState::Cancelled || finalState == TaskState::Succeeded)); }
            verifyTask(terminal, finalState);
            REQUIRE(fixture.task->calls == 1U);
            verifyResources(kernel, false);
            verifyState(kernel, 0U);
            // Prove released executor capacity and resource claims by running another task.
            // 中文翻译：实际执行下一任务，验证线程池容量和独占资源都已释放。
            auto next = request;
            next.requestId = id<RequestId>("request.stress.next");
            next.idempotencyKey.reset();
            const auto nextAccepted = take(kernel.execution().executeCommand(next));
            REQUIRE(nextAccepted.taskId.has_value());
            verifyTask(take(kernel.execution().waitTask(*nextAccepted.taskId, 5s)), TaskState::Succeeded);
            REQUIRE(fixture.task->calls == 2U);
            verifyResources(kernel, false);
            REQUIRE(kernel.documentRuntime().close(document));
            REQUIRE(kernel.documentRuntime().open(document));
            verifyState(kernel, 0U);
            REQUIRE(kernel.shutdown());
        }
        Fixture recovered{root, false, ExecutionScenario::Task};
        const auto accepted = take(recovered.kernel.execution().executeCommand(request));
        REQUIRE(accepted.replayed);
        REQUIRE(accepted.taskId == taskId);
        verifyTask(take(recovered.kernel.execution().waitTask(*taskId, 5s)), finalState);
        REQUIRE(recovered.task->calls == 0U);
        verifyResources(recovered.kernel, false);
        verifyState(recovered.kernel, 0U);
        REQUIRE(recovered.kernel.shutdown());
    }
}

WorkflowRequest stressWorkflow()
{
    return {id<WorkflowId>("workflow.stress.instance"), id<WorkflowName>("workflow.stress"), Value{Value::Object{}},
        session, project, document, id<CorrelationId>("correlation.stress"), id<TraceId>("trace.stress")};
}
void verifyWorkflow(const WorkflowSnapshot& snapshot, WorkflowState expected, std::size_t commits)
{
    REQUIRE(snapshot.state == expected);
    REQUIRE(snapshot.steps.size() == 2U);
    REQUIRE(snapshot.steps.front().state == WorkflowStepState::Succeeded);
    REQUIRE(snapshot.steps.front().attempt == 1U);
    REQUIRE(snapshot.steps.front().result == Value{Value::Object{{"id", Value{"object.stress.first"}}}});
    REQUIRE_FALSE(snapshot.steps.front().error.has_value());
    const auto& second = snapshot.steps.back();
    if(commits == 1U) {
        REQUIRE(second.state == WorkflowStepState::Cancelled);
        REQUIRE(second.attempt == 0U);
        REQUIRE_FALSE(second.result.has_value());
    } else {
        REQUIRE(second.state == WorkflowStepState::Succeeded);
        REQUIRE(second.attempt == 1U);
        REQUIRE(second.result == Value{Value::Object{{"id", Value{"object.stress.second"}}}});
    }
    if(expected == WorkflowState::Cancelled) {
        REQUIRE(snapshot.error.has_value());
        REQUIRE(std::string(snapshot.error->code.value()) == "Workflow.Cancelled");
    } else {
        REQUIRE_FALSE(snapshot.error.has_value());
        REQUIRE(snapshot.result == Value{Value::Object{}});
    }
}

void exerciseWorkflowCancellation(CancelOrder order)
{
    // Eight cancellation calls serialize durable checkpoints under the instance lock.
    // 中文翻译：八路取消会在实例锁下串行提交持久 checkpoint，等待预算须覆盖整批 I/O。
    constexpr auto durableBatchTimeout = 30s;
    for(unsigned int round = 0U; round < rounds; ++round) {
        const auto root = newRoot();
        INFO("round=" << round << " evidence=" << root.string());
        const auto request = stressWorkflow();
        WorkflowState finalState{WorkflowState::Failed};
        std::size_t commits = 0U;
        {
            Fixture fixture{root, true, ExecutionScenario::Workflow};
            auto& kernel = fixture.kernel;
            auto gate = std::make_shared<TimedGate>(durableBatchTimeout);
            fixture.command->gate = gate;
            TimedGate race{durableBatchTimeout};
            std::future<Result<WorkflowSnapshot>> advance;
            std::vector<std::future<Result<WorkflowSnapshot>>> cancels;
            std::future<void> finish;
            ReleaseOnExit releaseWorker{*gate};
            ReleaseOnExit releaseRace{race};
            REQUIRE(kernel.execution().startWorkflow(request));
            advance = std::async(std::launch::async, [&] { return kernel.execution().advanceWorkflow(request.workflowId); });
            REQUIRE(gate->awaitArrivals(1U));
            const auto running = take(kernel.execution().workflow(request.workflowId));
            REQUIRE(running.steps.front().state == WorkflowStepState::Running);
            REQUIRE(running.steps.front().attempt == 1U);
            const auto closed = kernel.documentRuntime().close(document);
            REQUIRE_FALSE(closed);
            REQUIRE(std::string(closed.error().code.value()) == "Document.ActiveOperations");
            const auto stopped = kernel.shutdown();
            REQUIRE_FALSE(stopped);
            REQUIRE(std::string(stopped.error().code.value()) == "Kernel.ActiveTransactions");
            REQUIRE(kernel.state() == AppKernelState::Ready);
            if(order == CancelOrder::AfterCompletion) {
                gate->release();
                REQUIRE(take(completed(advance, durableBatchTimeout)).state == WorkflowState::Succeeded);
            }
            const auto cancelStarted = std::chrono::steady_clock::now();
            for(unsigned int index = 0U; index < 8U; ++index) {
                cancels.push_back(std::async(std::launch::async, [&] {
                    if(order == CancelOrder::Race) { race.arriveAndWait(); }
                    return kernel.execution().cancelWorkflow(request.workflowId);
                }));
            }
            if(order == CancelOrder::Race) {
                finish = std::async(std::launch::async, [&] { race.arriveAndWait(); gate->release(); });
                REQUIRE(race.awaitArrivals(9U));
                race.release();
            }
            for(auto& future : cancels) {
                const auto cancelled = take(completed(future, durableBatchTimeout));
                REQUIRE((cancelled.state == WorkflowState::CancelRequested
                    || cancelled.state == WorkflowState::Cancelled || cancelled.state == WorkflowState::Succeeded));
            }
            const auto cancelElapsed = std::chrono::steady_clock::now() - cancelStarted;
            INFO("durable cancel batch ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(cancelElapsed).count());
            if(cancelElapsed >= 5s) {
                WARN("Durable cancel batch exceeded the former 5-second wait budget: "
                    << std::chrono::duration_cast<std::chrono::milliseconds>(cancelElapsed).count() << " ms");
            }
            if(order == CancelOrder::BeforeCompletion) {
                const auto cancelling = take(kernel.execution().workflow(request.workflowId));
                REQUIRE(cancelling.state == WorkflowState::CancelRequested);
                REQUIRE(cancelling.steps.front().state == WorkflowStepState::Running);
                REQUIRE(take(kernel.documents().snapshot(document)).objects().size() == 0U);
                gate->release();
            }
            if(finish.valid()) { completed(finish, durableBatchTimeout); }
            const auto terminal = advance.valid() ? take(completed(advance, durableBatchTimeout))
                : take(kernel.execution().workflow(request.workflowId));
            finalState = terminal.state;
            commits = terminal.steps.back().state == WorkflowStepState::Succeeded ? 2U : 1U;
            if(order == CancelOrder::BeforeCompletion) {
                REQUIRE(finalState == WorkflowState::Cancelled);
                REQUIRE(commits == 1U);
            } else if(order == CancelOrder::AfterCompletion) {
                REQUIRE(finalState == WorkflowState::Succeeded);
                REQUIRE(commits == 2U);
            } else { REQUIRE((finalState == WorkflowState::Cancelled || finalState == WorkflowState::Succeeded)); }
            verifyWorkflow(terminal, finalState, commits);
            REQUIRE(fixture.command->calls == commits);
            verifyState(kernel, commits);
            verifyWorkflow(take(kernel.execution().advanceWorkflow(request.workflowId)), finalState, commits);
            REQUIRE(fixture.command->calls == commits);
            REQUIRE(kernel.documentRuntime().close(document));
            REQUIRE(kernel.documentRuntime().open(document));
            verifyState(kernel, commits);
            REQUIRE(kernel.shutdown());
        }
        Fixture recovered{root, false, ExecutionScenario::Workflow};
        verifyWorkflow(take(recovered.kernel.execution().workflow(request.workflowId)), finalState, commits);
        verifyWorkflow(take(recovered.kernel.execution().cancelWorkflow(request.workflowId)), finalState, commits);
        verifyWorkflow(take(recovered.kernel.execution().advanceWorkflow(request.workflowId)), finalState, commits);
        REQUIRE(recovered.command->calls == 0U);
        verifyState(recovered.kernel, commits);
        REQUIRE(recovered.kernel.shutdown());
    }
}
} // namespace

TEST_CASE("Persistence Host rejects a second Kernel without abandoning a running command", "[persistence][host-session][concurrency]")
{
    const auto root = newRoot();
    INFO("Retained Host evidence: " << root.string());
    Fixture first{root};
    auto gate = std::make_shared<TimedGate>();
    first.command->gate = gate;
    auto request = createRequest();
    request.idempotencyKey = id<IdempotencyKey>("key.host.running");
    auto running = std::async(std::launch::async, [&] { return first.kernel.execution().executeCommand(request); });
    ReleaseOnExit release{*gate};
    REQUIRE(gate->awaitArrivals(1U));
    auto observer = take(SqlitePersistenceBackend::open({root / "state.db"}));
    const auto before = take(observer->query("SELECT * FROM command_idempotency"));
    REQUIRE(before.size() == 1U);
    REQUIRE(before.front().at("status") == Value{"pending"});
    AppKernel second;
    take(second.configurePersistence(take(SqlitePersistenceBackend::open({root / "state.db"})),
        std::make_shared<JsonconsAdapter>(), std::make_shared<Sha256HashService>(),
        take(FilesystemSnapshotStore::create({root / "snapshots", 1024U * 1024U}))));
    const auto booted = second.bootstrap();
    CHECK_FALSE(booted);
    CHECK(take(observer->query("SELECT * FROM command_idempotency")) == before);
    CHECK(first.kernel.state() == AppKernelState::Ready);
    gate->release();
    REQUIRE(completed(running));
    verifyState(first.kernel, 1U);
    CHECK(take(observer->query("SELECT status FROM command_idempotency")).front().at("status") == Value{"completed"});
    REQUIRE(first.kernel.shutdown());
}

TEST_CASE("Kernel task stress repeated cancellation preserves running ownership", "[runtime][stress][f3b]")
{ exerciseTaskCancellation(CancelOrder::BeforeCompletion); }
TEST_CASE("Kernel task stress cancellation after completion preserves success", "[runtime][stress][f3b]")
{ exerciseTaskCancellation(CancelOrder::AfterCompletion); }
TEST_CASE("Kernel task stress cancellation races completion without losing resources", "[runtime][stress][f3b]")
{ exerciseTaskCancellation(CancelOrder::Race); }

TEST_CASE("Kernel workflow stress running cancellation retains accepted commits", "[runtime][stress][f3b]")
{ exerciseWorkflowCancellation(CancelOrder::BeforeCompletion); }
TEST_CASE("Kernel workflow stress late cancellation preserves completed steps", "[runtime][stress][f3b]")
{ exerciseWorkflowCancellation(CancelOrder::AfterCompletion); }
TEST_CASE("Kernel workflow stress cancellation races command completion", "[runtime][stress][f3b]")
{ exerciseWorkflowCancellation(CancelOrder::Race); }

TEST_CASE("Kernel task stress shutdown timeout retains ownership until actual completion", "[runtime][stress][f3b]")
{
    for(unsigned int round = 0U; round < rounds; ++round) {
        const auto root = newRoot();
        INFO("round=" << round << " evidence=" << root.string());
        const auto request = taskCommand();
        std::optional<TaskId> taskId;
        {
            Fixture fixture{root, true, ExecutionScenario::Task};
            auto& kernel = fixture.kernel;
            ReleaseOnExit releaseWorker{fixture.task->gate};
            const auto accepted = take(kernel.execution().executeCommand(request));
            REQUIRE(accepted.taskId.has_value());
            taskId = accepted.taskId;
            REQUIRE(fixture.task->gate.awaitArrivals(1U));
            const auto stopped = kernel.shutdown(1ms);
            REQUIRE_FALSE(stopped);
            REQUIRE(std::string(stopped.error().code.value()) == "Task.ShutdownTimeout");
            REQUIRE(kernel.state() == AppKernelState::Stopping);
            for(unsigned int index = 0U; index < 8U; ++index) { REQUIRE(kernel.execution().cancelTask(*taskId)); }
            REQUIRE(take(kernel.execution().task(*taskId)).state == TaskState::CancelRequested);
            REQUIRE(kernel.scheduler().activeTaskCount() == 1U);
            verifyResources(kernel, true);
            REQUIRE_FALSE(kernel.execution().executeCommand(request));
            REQUIRE_FALSE(kernel.execution().executeQuery(queryRequest()));
            REQUIRE_FALSE(kernel.documentRuntime().close(document));
            fixture.task->gate.release();
            verifyTask(take(kernel.execution().waitTask(*taskId, 5s)), TaskState::Cancelled);
            REQUIRE(fixture.task->calls == 1U);
            verifyResources(kernel, false);
            verifyState(kernel, 0U);
            REQUIRE(kernel.shutdown());
            REQUIRE(kernel.state() == AppKernelState::Stopped);
        }
        Fixture recovered{root, false, ExecutionScenario::Task};
        verifyTask(take(recovered.kernel.execution().waitTask(*taskId, 5s)), TaskState::Cancelled);
        REQUIRE(recovered.task->calls == 0U);
        verifyResources(recovered.kernel, false);
        verifyState(recovered.kernel, 0U);
        REQUIRE(recovered.kernel.shutdown());
    }
}

TEST_CASE("Kernel concurrency queries block close and shutdown without mutating state", "[runtime][stress][f3a]")
{
    for(unsigned int round = 0U; round < rounds; ++round) {
        const auto root = newRoot();
        INFO("round=" << round << " evidence=" << root.string());
        Fixture fixture{root};
        auto& kernel = fixture.kernel;
        auto gate = std::make_shared<TimedGate>();
        fixture.query->gate = gate;
        std::vector<std::future<Result<QueryResponse>>> futures;
        ReleaseOnExit release{*gate};
        for(unsigned int index = 0U; index < 16U; ++index) {
            futures.push_back(std::async(std::launch::async, [&] { return kernel.execution().executeQuery(queryRequest()); }));
        }
        REQUIRE(gate->awaitArrivals(16U));
        REQUIRE(active(kernel, DocumentActivityKind::Query) == 16U);
        const auto closed = kernel.documentRuntime().close(document);
        REQUIRE_FALSE(closed);
        REQUIRE(std::string(closed.error().code.value()) == "Document.ActiveOperations");
        const auto stopped = kernel.shutdown();
        REQUIRE_FALSE(stopped);
        REQUIRE(std::string(stopped.error().code.value()) == "Kernel.ActiveExecutions");
        REQUIRE(kernel.state() == AppKernelState::Ready);
        gate->release();
        for(auto& future : futures) { verifyQuery(take(completed(future))); }
        REQUIRE(fixture.query->calls == 16U);
        verifyState(kernel, 0U);
        REQUIRE(kernel.documentRuntime().close(document));
        REQUIRE(kernel.documentRuntime().open(document));
        verifyState(kernel, 0U);
        REQUIRE(kernel.shutdown());
    }
}

TEST_CASE("Kernel concurrency identical command keys commit once and replay after restart", "[runtime][stress][f3a]")
{
    for(unsigned int round = 0U; round < rounds; ++round) {
        const auto root = newRoot();
        INFO("round=" << round << " evidence=" << root.string());
        auto request = createRequest();
        request.idempotencyKey = id<IdempotencyKey>("key.stress.same");
        std::optional<TransactionId> transaction;
        {
            Fixture fixture{root};
            auto& kernel = fixture.kernel;
            auto gate = std::make_shared<TimedGate>();
            fixture.command->gate = gate;
            std::vector<std::future<Result<CommandResponse>>> futures;
            ReleaseOnExit release{*gate};
            for(unsigned int index = 0U; index < 16U; ++index) {
                futures.push_back(std::async(std::launch::async, [&] { return kernel.execution().executeCommand(request); }));
            }
            REQUIRE(gate->awaitArrivals(1U));
            REQUIRE(awaitCondition([&] { return active(kernel, DocumentActivityKind::Command) == 16U; }));
            REQUIRE(fixture.command->calls == 1U);
            // The Transaction lease covers begin admission, not the candidate lifetime.
            // 中文翻译：Transaction 租约仅覆盖 begin 准入，候选生存期由活动事务表管理。
            REQUIRE(active(kernel, DocumentActivityKind::Transaction) == 0U);
            const auto closed = kernel.documentRuntime().close(document);
            REQUIRE_FALSE(closed);
            REQUIRE(std::string(closed.error().code.value()) == "Document.ActiveOperations");
            const auto stopped = kernel.shutdown();
            REQUIRE_FALSE(stopped);
            REQUIRE(std::string(stopped.error().code.value()) == "Kernel.ActiveTransactions");
            gate->release();
            unsigned int replayed = 0U;
            for(auto& future : futures) {
                const auto response = take(completed(future));
                REQUIRE(response.commit.has_value());
                if(!transaction) { transaction = response.commit->transactionId; }
                REQUIRE(response.commit->transactionId == transaction);
                REQUIRE(response.result == request.arguments);
                REQUIRE(response.postExecutionErrors.empty());
                replayed += response.replayed ? 1U : 0U;
            }
            REQUIRE(replayed == 15U);
            REQUIRE(fixture.command->calls == 1U);
            verifyState(kernel, 1U);
            REQUIRE(kernel.shutdown());
        }
        Fixture recovered{root, false};
        const auto response = take(recovered.kernel.execution().executeCommand(request));
        REQUIRE(response.replayed);
        REQUIRE(response.commit.has_value());
        REQUIRE(response.commit->transactionId == transaction);
        REQUIRE(response.result == request.arguments);
        REQUIRE(recovered.command->calls == 0U);
        verifyState(recovered.kernel, 1U);
        REQUIRE(recovered.kernel.shutdown());
    }
}

TEST_CASE("Kernel concurrency revision competitors leave exactly one durable winner", "[runtime][stress][f3a]")
{
    for(unsigned int round = 0U; round < rounds; ++round) {
        const auto root = newRoot();
        INFO("round=" << round << " evidence=" << root.string());
        std::optional<ObjectId> winner;
        {
            Fixture fixture{root};
            auto& kernel = fixture.kernel;
            auto gate = std::make_shared<TimedGate>();
            fixture.command->gate = gate;
            std::vector<std::future<Result<CommandResponse>>> futures;
            ReleaseOnExit release{*gate};
            for(unsigned int index = 0U; index < 8U; ++index) {
                futures.push_back(std::async(std::launch::async, [&, index] { return kernel.execution().executeCommand(createRequest(index)); }));
            }
            REQUIRE(gate->awaitArrivals(8U));
            REQUIRE(active(kernel, DocumentActivityKind::Command) == 8U);
            REQUIRE(active(kernel, DocumentActivityKind::Transaction) == 0U);
            const auto stopped = kernel.shutdown();
            REQUIRE_FALSE(stopped);
            REQUIRE(std::string(stopped.error().code.value()) == "Kernel.ActiveTransactions");
            REQUIRE(take(kernel.documents().snapshot(document)).objects().size() == 0U);
            gate->release();
            unsigned int failures = 0U;
            for(auto& future : futures) {
                const auto response = completed(future);
                if(response) {
                    REQUIRE_FALSE(winner.has_value());
                    REQUIRE(response.value().commit.has_value());
                    REQUIRE(response.value().commit->changes.size() == 1U);
                    winner = response.value().commit->changes.front().objectId;
                    REQUIRE(response.value().postExecutionErrors.empty());
                } else {
                    REQUIRE(std::string(response.error().code.value()) == "Project.RevisionConflict");
                    ++failures;
                }
            }
            REQUIRE(failures == 7U);
            REQUIRE(winner.has_value());
            REQUIRE(fixture.command->calls == 8U);
            REQUIRE(take(kernel.documents().snapshot(document)).objects().contains(*winner));
            verifyState(kernel, 1U);
            REQUIRE(kernel.shutdown());
        }
        Fixture recovered{root, false};
        REQUIRE(take(recovered.kernel.documents().snapshot(document)).objects().contains(*winner));
        verifyState(recovered.kernel, 1U);
        REQUIRE(recovered.command->calls == 0U);
        REQUIRE(recovered.kernel.shutdown());
    }
}

TEST_CASE("Kernel concurrency closing excludes query admission and permits clean reopen", "[runtime][stress][f3a]")
{
    for(unsigned int round = 0U; round < rounds; ++round) {
        const auto root = newRoot();
        INFO("round=" << round << " evidence=" << root.string());
        Fixture fixture{root};
        auto& kernel = fixture.kernel;
        fixture.store->armed = true;
        auto close = std::async(std::launch::async, [&] { return kernel.documentRuntime().close(document); });
        std::vector<std::future<Result<QueryResponse>>> queries;
        ReleaseOnExit release{fixture.store->gate};
        REQUIRE(fixture.store->gate.awaitArrivals(1U));
        REQUIRE(take(kernel.documentRuntime().lifecycle(document)).state == DocumentLifecycleState::Closing);
        for(unsigned int index = 0U; index < 16U; ++index) {
            queries.push_back(std::async(std::launch::async, [&] { return kernel.execution().executeQuery(queryRequest()); }));
        }
        for(auto& future : queries) {
            const auto result = completed(future);
            REQUIRE_FALSE(result);
            REQUIRE(std::string(result.error().code.value()) == "Document.NotOpen");
        }
        REQUIRE(fixture.query->calls == 0U);
        fixture.store->gate.release();
        REQUIRE(take(completed(close)).state == DocumentLifecycleState::Detached);
        REQUIRE(kernel.documentRuntime().open(document));
        verifyQuery(take(kernel.execution().executeQuery(queryRequest())));
        REQUIRE(fixture.query->calls == 1U);
        verifyState(kernel, 0U);
        REQUIRE(kernel.shutdown());
    }
}

TEST_CASE("ProjectRuntime seals all child admission during coordinated durable close", "[project-runtime][concurrency]")
{
    for(unsigned int round = 0U; round < rounds; ++round) {
        const auto root = newRoot();
        INFO("round=" << round << " evidence=" << root.string());
        Fixture fixture{root};
        auto& kernel = fixture.kernel;
        const auto sibling = id<DocumentId>("document.sibling");
        const auto other = id<ProjectId>("project.other");
        const auto foreign = id<DocumentId>("document.foreign");
        REQUIRE(kernel.documentRuntime().create(project, sibling));
        REQUIRE(kernel.projectRuntime().create(other));
        REQUIRE(kernel.documentRuntime().create(other, foreign));
        fixture.store->armed = true;
        auto close = std::async(std::launch::async, [&] { return kernel.projectRuntime().close(project); });
        ReleaseOnExit release{fixture.store->gate};
        REQUIRE(fixture.store->gate.awaitArrivals(1U));
        REQUIRE(take(kernel.projectRuntime().lifecycle(project)).state == ProjectLifecycleState::Closing);
        REQUIRE_FALSE(kernel.projectRuntime().open(project));
        REQUIRE_FALSE(kernel.projectRuntime().close(project));
        REQUIRE_FALSE(kernel.documentRuntime().create(project, id<DocumentId>("document.rejected")));
        REQUIRE_FALSE(kernel.documentRuntime().attach({project, id<DocumentId>("document.attach-rejected"), {}, {}}));
        REQUIRE_FALSE(kernel.documentRuntime().snapshot(document));
        REQUIRE_FALSE(kernel.documentRuntime().snapshot(sibling));
        REQUIRE_FALSE(kernel.documentRuntime().remove(sibling));
        REQUIRE_FALSE(kernel.documentRuntime().close(sibling));
        REQUIRE_FALSE(kernel.execution().executeQuery(queryRequest()));
        REQUIRE(fixture.query->calls == 0U);
        REQUIRE(kernel.documentRuntime().snapshot(foreign));
        fixture.store->gate.release();
        REQUIRE(take(completed(close)).state == ProjectLifecycleState::Closed);
        REQUIRE_FALSE(kernel.documents().contains(document));
        REQUIRE_FALSE(kernel.documents().contains(sibling));
        REQUIRE(kernel.documents().contains(foreign));
        REQUIRE(kernel.projectRuntime().open(project));
        REQUIRE_FALSE(kernel.documents().contains(document));
        REQUIRE(kernel.documentRuntime().open(document));
        verifyQuery(take(kernel.execution().executeQuery(queryRequest())));
        REQUIRE(kernel.shutdown());
    }
}
