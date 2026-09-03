#include <lasercnc/infrastructure/bs_thread_pool_executor.hpp>
#include <lasercnc/kernel/app_kernel.hpp>
#include <lasercnc/observability/log_service.hpp>
#include <lasercnc/runtime/resource_manager.hpp>
#include <lasercnc/runtime/scheduler.hpp>
#include <lasercnc/runtime/task_registry.hpp>
#include <lasercnc/runtime/task_runtime.hpp>
#include <lasercnc/runtime/transaction_manager.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using namespace lasercnc::foundation;
using namespace lasercnc::infrastructure;
using namespace lasercnc::kernel;
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

Schema schema(const char* id)
{
    auto created = Schema::create(
        validId<SchemaId>(id), Version {1U, 0U, 0U}, SchemaKind::Any);
    if(!created) {
        throw std::logic_error("Invalid test schema");
    }
    return std::move(created).value();
}

TaskDescriptor descriptor(const char* name)
{
    return TaskDescriptor {
        validId<TaskName>(name),
        Version {1U, 0U, 0U},
        schema("schema.task.input"),
        schema("schema.task.result")};
}

TaskRequest request(const char* id, const char* name)
{
    return TaskRequest {
        validId<TaskId>(id),
        validId<TaskName>(name),
        Value {},
        validId<TraceId>("trace.task")};
}

class LambdaHandler final : public ITaskHandler {
public:
    using Function = std::function<Result<Value>(const TaskRequest&, const TaskContext&)>;

    explicit LambdaHandler(Function function)
        : function_(std::move(function))
    {
    }

    Result<Value> execute(const TaskRequest& taskRequest, const TaskContext& context) override
    {
        return function_(taskRequest, context);
    }

private:
    Function function_;
};

class PassValidator final : public ISchemaValidator {
public:
    Result<void> validate(const Schema&, const Value&) const override
    {
        return Result<void>::success();
    }
};

class NullLog final : public lasercnc::observability::ILogService {
public:
    Result<void> write(const lasercnc::observability::LogRecord&) override
    {
        return Result<void>::success();
    }
    Result<void> flush() override { return Result<void>::success(); }
};

struct RuntimeFixture final {
    ResourceManager resources;
    Scheduler scheduler{resources};
    TaskRegistry registry;
    ExecutionServices services;
    lasercnc::state::DocumentStore documents;
    std::unique_ptr<BsThreadPoolExecutor> executor;
    TaskRuntime runtime{registry, scheduler, services, documents};

    explicit RuntimeFixture(std::size_t threads = 2U)
    {
        auto created = BsThreadPoolExecutor::create(BsThreadPoolExecutorOptions {threads});
        if(!created) {
            throw std::runtime_error("Executor creation failed");
        }
        executor = std::move(created).value();
        REQUIRE(services
                    .configure(std::make_shared<PassValidator>(), std::make_shared<NullLog>())
                    .hasValue());
        REQUIRE(scheduler.configureExecutor(*executor).hasValue());
    }

    void start()
    {
        REQUIRE(scheduler.start().hasValue());
        runtime.start();
    }

    void stop()
    {
        runtime.stop();
        REQUIRE(scheduler.shutdown(2s).hasValue());
    }
};

} // namespace

TEST_CASE("TaskRuntime enforces registry identity and monotonic progress", "[runtime][task]")
{
    RuntimeFixture fixture;
    std::atomic_bool sawRegression {false};
    auto handler = std::make_shared<LambdaHandler>(
        [&sawRegression](const TaskRequest&, const TaskContext& context) {
            REQUIRE(context.progress.report(0.25, "started").hasValue());
            const auto regressed = context.progress.report(0.2, "invalid");
            sawRegression.store(!regressed.hasValue()
                && std::string(regressed.error().code.value()) == "Task.ProgressRegression");
            REQUIRE(context.progress.report(0.75, "nearly-done").hasValue());
            return Result<Value>::success(Value {"done"});
        });
    REQUIRE(fixture.registry.registerHandler(descriptor("task.progress"), handler).hasValue());
    fixture.start();

    const auto task = request("task-1", "task.progress");
    REQUIRE(fixture.runtime.submit(task).hasValue());
    auto completed = fixture.runtime.wait(task.taskId, 2s);
    REQUIRE(completed.hasValue());
    CHECK(completed.value().state == TaskState::Succeeded);
    CHECK(completed.value().progress == 1.0);
    CHECK(completed.value().traceId == task.traceId);
    REQUIRE(completed.value().result.has_value());
    REQUIRE(completed.value().result->getIf<std::string>() != nullptr);
    CHECK(*completed.value().result->getIf<std::string>() == "done");
    CHECK(sawRegression.load());
    fixture.stop();
}

TEST_CASE("TaskRuntime cancellation and deadline are cooperative", "[runtime][task]")
{
    RuntimeFixture fixture;
    auto started = std::make_shared<std::promise<void>>();
    auto handler = std::make_shared<LambdaHandler>(
        [started](const TaskRequest&, const TaskContext& context) {
            started->set_value();
            while(!context.cancellation.cancellationRequested()) {
                std::this_thread::yield();
            }
            return Result<Value>::success(Value {});
        });
    REQUIRE(fixture.registry.registerHandler(descriptor("task.cancel"), handler).hasValue());
    fixture.start();

    auto cancellable = request("task-cancel", "task.cancel");
    REQUIRE(fixture.runtime.submit(cancellable).hasValue());
    started->get_future().wait();
    REQUIRE(fixture.runtime.cancel(cancellable.taskId).hasValue());
    auto cancelled = fixture.runtime.wait(cancellable.taskId, 2s);
    REQUIRE(cancelled.hasValue());
    CHECK(cancelled.value().state == TaskState::Cancelled);
    REQUIRE(cancelled.value().error.has_value());
    CHECK(cancelled.value().error->category == ErrorCategory::Cancellation);

    auto deadline = request("task-deadline", "task.cancel");
    deadline.deadline = std::chrono::steady_clock::now() - 1ms;
    REQUIRE(fixture.runtime.submit(deadline).hasValue());
    auto expired = fixture.runtime.wait(deadline.taskId, 2s);
    REQUIRE(expired.hasValue());
    CHECK(expired.value().state == TaskState::Cancelled);
    REQUIRE(expired.value().error.has_value());
    CHECK(std::string(expired.value().error->code.value()) == "Task.DeadlineExceeded");
    fixture.stop();
}

TEST_CASE("Scheduler arbitrates project read and write claims", "[runtime][task][resource]")
{
    RuntimeFixture fixture(2U);
    auto writerStarted = std::make_shared<std::promise<void>>();
    auto releaseWriter = std::make_shared<std::shared_future<void>>();
    auto releasePromise = std::make_shared<std::promise<void>>();
    *releaseWriter = releasePromise->get_future().share();
    std::atomic_bool readerStarted {false};
    std::atomic_bool otherStarted {false};

    REQUIRE(fixture.registry
                .registerHandler(
                    descriptor("task.writer"),
                    std::make_shared<LambdaHandler>(
                        [writerStarted, releaseWriter](const TaskRequest&, const TaskContext&) {
                            writerStarted->set_value();
                            releaseWriter->wait();
                            return Result<Value>::success(Value {});
                        }))
                .hasValue());
    REQUIRE(fixture.registry
                .registerHandler(
                    descriptor("task.other"),
                    std::make_shared<LambdaHandler>(
                        [&otherStarted](const TaskRequest&, const TaskContext&) {
                            otherStarted.store(true);
                            return Result<Value>::success(Value {});
                        }))
                .hasValue());
    REQUIRE(fixture.registry
                .registerHandler(
                    descriptor("task.reader"),
                    std::make_shared<LambdaHandler>(
                        [&readerStarted](const TaskRequest&, const TaskContext&) {
                            readerStarted.store(true);
                            return Result<Value>::success(Value {});
                        }))
                .hasValue());
    fixture.start();

    const auto project = validId<ResourceId>("project.alpha");
    auto writer = request("writer", "task.writer");
    writer.resources.push_back(
        ResourceClaim {ResourceKind::ProjectWrite, project, ResourceAccess::Exclusive, 1U});
    auto reader = request("reader", "task.reader");
    reader.priority = 10;
    reader.resources.push_back(
        ResourceClaim {ResourceKind::ProjectRead, project, ResourceAccess::Shared, 1U});
    auto other = request("other", "task.other");
    other.priority = 1;
    other.resources.push_back(ResourceClaim {
        ResourceKind::ProjectRead,
        validId<ResourceId>("project.beta"),
        ResourceAccess::Shared,
        1U});

    REQUIRE(fixture.runtime.submit(writer).hasValue());
    writerStarted->get_future().wait();
    REQUIRE(fixture.runtime.submit(reader).hasValue());
    REQUIRE(fixture.runtime.submit(other).hasValue());
    auto otherDone = fixture.runtime.wait(other.taskId, 2s);
    REQUIRE(otherDone.hasValue());
    CHECK(otherDone.value().state == TaskState::Succeeded);
    CHECK(otherStarted.load());
    std::this_thread::sleep_for(30ms);
    CHECK_FALSE(readerStarted.load());
    auto waiting = fixture.runtime.snapshot(reader.taskId);
    REQUIRE(waiting.hasValue());
    CHECK(waiting.value().state == TaskState::Ready);

    releasePromise->set_value();
    REQUIRE(fixture.runtime.wait(writer.taskId, 2s).hasValue());
    auto readDone = fixture.runtime.wait(reader.taskId, 2s);
    REQUIRE(readDone.hasValue());
    CHECK(readDone.value().state == TaskState::Succeeded);
    CHECK(readerStarted.load());
    fixture.stop();
}

TEST_CASE("TaskRuntime captures immutable document revisions and marks stale results", "[runtime][task][revision]")
{
    RuntimeFixture fixture(1U);
    const auto project = validId<ProjectId>("project.snapshot");
    const auto document = validId<DocumentId>("document.snapshot");
    REQUIRE(fixture.documents.addDocument(project, document).hasValue());
    auto entered = std::make_shared<std::promise<void>>();
    auto releasePromise = std::make_shared<std::promise<void>>();
    auto release = std::make_shared<std::shared_future<void>>(releasePromise->get_future().share());
    std::atomic_bool sawSnapshot {false};
    REQUIRE(fixture.registry
                .registerHandler(
                    descriptor("task.snapshot"),
                    std::make_shared<LambdaHandler>(
                        [entered, release, &sawSnapshot](const TaskRequest&, const TaskContext& context) {
                            sawSnapshot.store(
                                context.document.has_value()
                                && context.document->revisions().at(RevisionScope::Document)
                                    == Revision {0U});
                            entered->set_value();
                            release->wait();
                            return Result<Value>::success(Value {"computed"});
                        }))
                .hasValue());
    fixture.start();

    auto task = request("snapshot", "task.snapshot");
    task.projectId = project;
    task.documentId = document;
    task.expectedRevisions = RevisionSet {};
    REQUIRE(fixture.runtime.submit(task).hasValue());
    entered->get_future().wait();

    TransactionManager transactions(fixture.documents);
    auto transaction = transactions.begin(validId<TransactionId>("transaction.stale"), document);
    REQUIRE(transaction.hasValue());
    REQUIRE(transaction.value()
                ->createObject(ObjectRecord {
                    validId<ObjectId>("object.stale-source"),
                    validId<ObjectTypeId>("kernel.task.test"),
                    Value {"changed"}})
                .hasValue());
    REQUIRE(transaction.value()->touchRevision(RevisionScope::Geometry).hasValue());
    REQUIRE(transaction.value()->commit().hasValue());
    releasePromise->set_value();

    auto completed = fixture.runtime.wait(task.taskId, 2s);
    REQUIRE(completed.hasValue());
    CHECK(completed.value().state == TaskState::Stale);
    CHECK(sawSnapshot.load());
    REQUIRE(completed.value().sourceRevisions.has_value());
    CHECK(completed.value().sourceRevisions->at(RevisionScope::Document) == Revision {0U});
    REQUIRE(completed.value().error.has_value());
    CHECK(std::string(completed.value().error->code.value()) == "Task.SourceRevisionChanged");
    fixture.stop();
}

TEST_CASE("Scheduler honors priority FIFO and dependency outcome", "[runtime][task][scheduler]")
{
    RuntimeFixture fixture(1U);
    auto blockerStarted = std::make_shared<std::promise<void>>();
    auto releasePromise = std::make_shared<std::promise<void>>();
    auto release = std::make_shared<std::shared_future<void>>(releasePromise->get_future().share());
    auto order = std::make_shared<std::vector<std::string>>();
    auto orderMutex = std::make_shared<std::mutex>();

    REQUIRE(fixture.registry
                .registerHandler(
                    descriptor("task.block"),
                    std::make_shared<LambdaHandler>(
                        [blockerStarted, release](const TaskRequest&, const TaskContext&) {
                            blockerStarted->set_value();
                            release->wait();
                            return Result<Value>::success(Value {});
                        }))
                .hasValue());
    REQUIRE(fixture.registry
                .registerHandler(
                    descriptor("task.order"),
                    std::make_shared<LambdaHandler>(
                        [order, orderMutex](const TaskRequest& task, const TaskContext&) {
                            std::lock_guard lock(*orderMutex);
                            order->emplace_back(task.taskId.value());
                            return Result<Value>::success(Value {});
                        }))
                .hasValue());
    REQUIRE(fixture.registry
                .registerHandler(
                    descriptor("task.fail"),
                    std::make_shared<LambdaHandler>(
                        [](const TaskRequest&, const TaskContext&) {
                            return Result<Value>::failure(makeError(
                                "Task.ExpectedFailure", ErrorCategory::Internal, "expected"));
                        }))
                .hasValue());
    fixture.start();

    auto blocker = request("blocker", "task.block");
    REQUIRE(fixture.runtime.submit(blocker).hasValue());
    blockerStarted->get_future().wait();
    auto low = request("low", "task.order");
    low.priority = 1;
    auto highFirst = request("high-first", "task.order");
    highFirst.priority = 10;
    auto highSecond = request("high-second", "task.order");
    highSecond.priority = 10;
    REQUIRE(fixture.runtime.submit(low).hasValue());
    REQUIRE(fixture.runtime.submit(highFirst).hasValue());
    REQUIRE(fixture.runtime.submit(highSecond).hasValue());
    releasePromise->set_value();
    REQUIRE(fixture.runtime.wait(low.taskId, 2s).hasValue());
    REQUIRE(order->size() == 3U);
    CHECK((*order)[0] == "high-first");
    CHECK((*order)[1] == "high-second");
    CHECK((*order)[2] == "low");

    auto failed = request("failed", "task.fail");
    REQUIRE(fixture.runtime.submit(failed).hasValue());
    auto dependent = request("dependent", "task.order");
    dependent.dependencies.push_back(failed.taskId);
    REQUIRE(fixture.runtime.submit(dependent).hasValue());
    auto stale = fixture.runtime.wait(dependent.taskId, 2s);
    REQUIRE(stale.hasValue());
    CHECK(stale.value().state == TaskState::Stale);
    fixture.stop();
}

TEST_CASE("Scheduler bounded shutdown reports non-cooperative work", "[runtime][task][shutdown]")
{
    RuntimeFixture fixture(1U);
    auto handler = std::make_shared<LambdaHandler>(
        [](const TaskRequest&, const TaskContext&) {
            std::this_thread::sleep_for(120ms);
            return Result<Value>::success(Value {});
        });
    REQUIRE(fixture.registry.registerHandler(descriptor("task.slow"), handler).hasValue());
    fixture.start();
    auto slow = request("slow", "task.slow");
    REQUIRE(fixture.runtime.submit(slow).hasValue());
    std::this_thread::sleep_for(10ms);
    fixture.runtime.stop();

    auto timedOut = fixture.scheduler.shutdown(5ms);
    REQUIRE_FALSE(timedOut.hasValue());
    CHECK(std::string(timedOut.error().code.value()) == "Task.ShutdownTimeout");
    auto cancelled = fixture.runtime.wait(slow.taskId, 2s);
    REQUIRE(cancelled.hasValue());
    CHECK(cancelled.value().state == TaskState::Cancelled);
    CHECK(fixture.scheduler.shutdown(2s).hasValue());
}

TEST_CASE("AppKernel owns freezes and stops the task stack", "[kernel][runtime][task]")
{
    lasercnc::kernel::AppKernel kernel;
    REQUIRE(kernel.executionServices()
                .configure(std::make_shared<PassValidator>(), std::make_shared<NullLog>())
                .hasValue());
    REQUIRE(kernel.taskRegistry()
                .registerHandler(
                    descriptor("task.kernel-owned"),
                    std::make_shared<LambdaHandler>(
                        [](const TaskRequest&, const TaskContext&) {
                            return Result<Value>::success(Value {"owned"});
                        }))
                .hasValue());
    auto created = BsThreadPoolExecutor::create(BsThreadPoolExecutorOptions {1U});
    REQUIRE(created.hasValue());
    REQUIRE(kernel.configureTaskExecutor(std::move(created).value()).hasValue());
    REQUIRE(kernel.bootstrap().hasValue());
    CHECK(kernel.taskRegistry().frozen());
    CHECK(kernel.resources().frozen());

    auto task = request("kernel-owned", "task.kernel-owned");
    REQUIRE(kernel.tasks().submit(task).hasValue());
    auto completed = kernel.tasks().wait(task.taskId, 2s);
    REQUIRE(completed.hasValue());
    CHECK(completed.value().state == TaskState::Succeeded);
    REQUIRE(kernel.shutdown().hasValue());
    CHECK(kernel.state() == lasercnc::kernel::AppKernelState::Stopped);
    CHECK_FALSE(kernel.tasks().submit(request("late", "task.kernel-owned")).hasValue());
}
