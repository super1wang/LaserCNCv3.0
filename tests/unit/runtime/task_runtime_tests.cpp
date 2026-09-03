#include <lasercnc/infrastructure/bs_thread_pool_executor.hpp>
#include <lasercnc/kernel/app_kernel.hpp>
#include <lasercnc/observability/log_service.hpp>
#include <lasercnc/observability/metrics_service.hpp>
#include <lasercnc/observability/trace_service.hpp>
#include <lasercnc/runtime/resource_manager.hpp>
#include <lasercnc/runtime/scheduler.hpp>
#include <lasercnc/runtime/task_registry.hpp>
#include <lasercnc/runtime/task_runtime.hpp>
#include <lasercnc/runtime/transaction_manager.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <array>
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

class AsyncPlanHandler final : public IAsyncCommandHandler {
public:
    Result<AsyncCommandPlan> prepare(const CommandRequest& command) override
    {
        calls.fetch_add(1U);
        auto task = request(
            ("task-" + std::string(command.requestId.value())).c_str(),
            "task.async-compute");
        return Result<AsyncCommandPlan>::success(AsyncCommandPlan {
            std::move(task),
            Value {Value::Object {{"accepted", Value {true}}}}});
    }

    std::atomic_size_t calls{0U};
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

class FailingTraceExporter final : public lasercnc::observability::ITraceExporter {
public:
    Result<void> exportSpan(const lasercnc::observability::TraceSpanRecord&) override
    {
        throw std::runtime_error("expected trace exporter exception");
    }
};

class FailingMetricsExporter final : public lasercnc::observability::IMetricsExporter {
public:
    Result<void> exportObservation(
        const lasercnc::observability::MetricObservation&) override
    {
        throw std::runtime_error("expected metrics exporter exception");
    }
};

const lasercnc::observability::TraceSpanRecord* findSpan(
    const std::vector<lasercnc::observability::TraceSpanRecord>& records,
    const char* name)
{
    const auto found = std::find_if(records.begin(), records.end(), [name](const auto& record) {
        return record.name == name;
    });
    return found == records.end() ? nullptr : &*found;
}

struct RuntimeFixture final {
    ResourceManager resources;
    lasercnc::observability::LocalTraceService traces;
    lasercnc::observability::LocalMetricsService metrics;
    Scheduler scheduler{resources, traces, metrics};
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
    const auto spans = fixture.traces.records();
    REQUIRE(spans.size() == 1U);
    CHECK(spans[0].status == lasercnc::observability::TraceStatus::Cancelled);
    fixture.stop();
}

TEST_CASE("TaskRuntime exposes pending running and cancel-requested transitions", "[runtime][task][state]")
{
    RuntimeFixture fixture(1U);
    auto entered = std::make_shared<std::promise<void>>();
    auto releasePromise = std::make_shared<std::promise<void>>();
    auto release = std::make_shared<std::shared_future<void>>(releasePromise->get_future().share());
    REQUIRE(fixture.registry
                .registerHandler(
                    descriptor("task.state"),
                    std::make_shared<LambdaHandler>(
                        [entered, release](const TaskRequest&, const TaskContext&) {
                            entered->set_value();
                            release->wait();
                            return Result<Value>::success(Value {});
                        }))
                .hasValue());
    fixture.start();

    auto running = request("state-running", "task.state");
    REQUIRE(fixture.runtime.submit(running).hasValue());
    entered->get_future().wait();
    auto runningSnapshot = fixture.runtime.snapshot(running.taskId);
    REQUIRE(runningSnapshot.hasValue());
    CHECK(runningSnapshot.value().state == TaskState::Running);

    auto dependent = request("state-pending", "task.state");
    dependent.dependencies.push_back(running.taskId);
    REQUIRE(fixture.runtime.submit(dependent).hasValue());
    auto pendingSnapshot = fixture.runtime.snapshot(dependent.taskId);
    REQUIRE(pendingSnapshot.hasValue());
    CHECK(pendingSnapshot.value().state == TaskState::Pending);

    REQUIRE(fixture.runtime.cancel(running.taskId).hasValue());
    auto cancellingSnapshot = fixture.runtime.snapshot(running.taskId);
    REQUIRE(cancellingSnapshot.hasValue());
    CHECK(cancellingSnapshot.value().state == TaskState::CancelRequested);
    REQUIRE(fixture.runtime.cancel(dependent.taskId).hasValue());
    auto pendingCancelled = fixture.runtime.snapshot(dependent.taskId);
    REQUIRE(pendingCancelled.hasValue());
    CHECK(pendingCancelled.value().state == TaskState::Cancelled);

    releasePromise->set_value();
    auto cancelled = fixture.runtime.wait(running.taskId, 2s);
    REQUIRE(cancelled.hasValue());
    CHECK(cancelled.value().state == TaskState::Cancelled);
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
    const auto spans = fixture.traces.records();
    const auto* span = findSpan(spans, "task.execute");
    REQUIRE(span != nullptr);
    CHECK(span->status == lasercnc::observability::TraceStatus::Stale);
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
    CHECK(kernel.traces().frozen());
    CHECK(kernel.metrics().frozen());
    CHECK(kernel.diagnostics().frozen());
    CHECK(kernel.persistence().frozen());

    auto task = request("kernel-owned", "task.kernel-owned");
    REQUIRE(kernel.tasks().submit(task).hasValue());
    auto completed = kernel.tasks().wait(task.taskId, 2s);
    REQUIRE(completed.hasValue());
    CHECK(completed.value().state == TaskState::Succeeded);
    REQUIRE(kernel.shutdown().hasValue());
    CHECK(kernel.state() == lasercnc::kernel::AppKernelState::Stopped);
    CHECK_FALSE(kernel.tasks().submit(request("late", "task.kernel-owned")).hasValue());
}

TEST_CASE("AppKernel preserves stopping state after bounded task shutdown timeout", "[kernel][runtime][task][shutdown]")
{
    lasercnc::kernel::AppKernel kernel;
    REQUIRE(kernel.executionServices()
                .configure(std::make_shared<PassValidator>(), std::make_shared<NullLog>())
                .hasValue());
    auto entered = std::make_shared<std::promise<void>>();
    REQUIRE(kernel.taskRegistry()
                .registerHandler(
                    descriptor("task.kernel-slow"),
                    std::make_shared<LambdaHandler>(
                        [entered](const TaskRequest&, const TaskContext&) {
                            entered->set_value();
                            std::this_thread::sleep_for(100ms);
                            return Result<Value>::success(Value {});
                        }))
                .hasValue());
    auto executor = BsThreadPoolExecutor::create(BsThreadPoolExecutorOptions {1U});
    REQUIRE(executor.hasValue());
    REQUIRE(kernel.configureTaskExecutor(std::move(executor).value()).hasValue());
    REQUIRE(kernel.bootstrap().hasValue());
    auto slow = request("kernel-slow", "task.kernel-slow");
    REQUIRE(kernel.tasks().submit(slow).hasValue());
    entered->get_future().wait();

    auto timedOut = kernel.shutdown(5ms);
    REQUIRE_FALSE(timedOut.hasValue());
    CHECK(std::string(timedOut.error().code.value()) == "Task.ShutdownTimeout");
    CHECK(kernel.state() == lasercnc::kernel::AppKernelState::Stopping);
    auto completed = kernel.tasks().wait(slow.taskId, 2s);
    REQUIRE(completed.hasValue());
    CHECK(completed.value().state == TaskState::Cancelled);
    REQUIRE(kernel.shutdown(2s).hasValue());
    CHECK(kernel.state() == lasercnc::kernel::AppKernelState::Stopped);
}

TEST_CASE("Asynchronous commands use CommandRuntime to accept one read-only task", "[runtime][command][task]")
{
    lasercnc::kernel::AppKernel kernel;
    const auto project = validId<ProjectId>("project.async-command");
    const auto document = validId<DocumentId>("document.async-command");
    const auto session = validId<SessionId>("session.async-command");
    const auto capability = validId<CapabilityId>("task.submit");
    REQUIRE(kernel.addDocument(project, document).hasValue());
    REQUIRE(kernel.executionServices()
                .configure(std::make_shared<PassValidator>(), std::make_shared<NullLog>())
                .hasValue());
    REQUIRE(kernel.traces().addExporter(std::make_shared<FailingTraceExporter>()).hasValue());
    REQUIRE(kernel.metrics().addExporter(std::make_shared<FailingMetricsExporter>()).hasValue());
    const std::array grants {capability};
    REQUIRE(kernel.capabilities().replace(session, grants).hasValue());

    std::atomic_bool receivedCommandContext {false};
    REQUIRE(kernel.taskRegistry()
                .registerHandler(
                    descriptor("task.async-compute"),
                    std::make_shared<LambdaHandler>(
                        [&receivedCommandContext, project, document](
                            const TaskRequest& task,
                            const TaskContext& context) {
                            receivedCommandContext.store(
                                task.projectId == project && task.documentId == document
                                && task.correlationId
                                    == validId<CorrelationId>("correlation.async-command")
                                && context.traceId == validId<TraceId>("trace.async-command")
                                && context.spanId.has_value()
                                && context.document.has_value());
                            return Result<Value>::success(Value {"computed"});
                        }))
                .hasValue());
    auto asyncHandler = std::make_shared<AsyncPlanHandler>();
    auto asyncDescriptor = CommandDescriptor {
        validId<CommandName>("command.async-compute"),
        Version {1U, 0U, 0U},
        schema("schema.async.arguments"),
        schema("schema.async.acceptance"),
        ExecutionMode::Asynchronous,
        SideEffectLevel::ReadOnly,
        capability,
        false,
        true,
        true};
    REQUIRE(kernel.commandRegistry()
                .registerAsyncHandler(asyncDescriptor, asyncHandler)
                .hasValue());
    auto unsafeDescriptor = asyncDescriptor;
    unsafeDescriptor.name = validId<CommandName>("command.async-write");
    unsafeDescriptor.sideEffect = SideEffectLevel::DocumentWrite;
    auto unsafe = kernel.commandRegistry().registerAsyncHandler(unsafeDescriptor, asyncHandler);
    REQUIRE_FALSE(unsafe.hasValue());
    CHECK(std::string(unsafe.error().code.value()) == "Command.AsyncSideEffectUnsupported");

    auto executor = BsThreadPoolExecutor::create(BsThreadPoolExecutorOptions {1U});
    REQUIRE(executor.hasValue());
    REQUIRE(kernel.configureTaskExecutor(std::move(executor).value()).hasValue());
    REQUIRE(kernel.bootstrap().hasValue());

    const auto key = validId<IdempotencyKey>("idempotency.async-command");
    auto command = CommandRequest {
        validId<RequestId>("request.async-first"),
        ExecutionContext {session, project, document},
        validId<CommandName>("command.async-compute"),
        Version {1U, 0U, 0U},
        Value {Value::Object {}},
        Revision {0U},
        validId<CorrelationId>("correlation.async-command"),
        validId<TraceId>("trace.async-command"),
        key,
        validId<SpanId>("span.external-parent")};
    auto accepted = kernel.commands().execute(command);
    REQUIRE(accepted.hasValue());
    CHECK_FALSE(accepted.value().commit.has_value());
    REQUIRE(accepted.value().taskId.has_value());
    CHECK(accepted.value().postExecutionErrors.empty());
    auto completed = kernel.tasks().wait(*accepted.value().taskId, 2s);
    REQUIRE(completed.hasValue());
    CHECK(completed.value().state == TaskState::Succeeded);
    CHECK(receivedCommandContext.load());

    const auto records = kernel.traces().records();
    REQUIRE(records.size() == 2U);
    const auto* commandSpan = findSpan(records, "command.execute");
    const auto* taskSpan = findSpan(records, "task.execute");
    REQUIRE(commandSpan != nullptr);
    REQUIRE(taskSpan != nullptr);
    CHECK(commandSpan->traceId == validId<TraceId>("trace.async-command"));
    REQUIRE(commandSpan->parentSpanId.has_value());
    CHECK(*commandSpan->parentSpanId == validId<SpanId>("span.external-parent"));
    REQUIRE(taskSpan->parentSpanId.has_value());
    CHECK(*taskSpan->parentSpanId == commandSpan->spanId);
    CHECK(taskSpan->traceId == commandSpan->traceId);
    CHECK(commandSpan->status == lasercnc::observability::TraceStatus::Succeeded);
    CHECK(taskSpan->status == lasercnc::observability::TraceStatus::Succeeded);
    CHECK(kernel.traces().exporterFailures().size() == 2U);
    CHECK(kernel.metrics().exporterFailures().size() == 4U);

    const auto metricSnapshot = kernel.metrics().snapshot();
    CHECK(std::count_if(
              metricSnapshot.begin(), metricSnapshot.end(), [](const auto& metric) {
                  return metric.name == validId<MetricName>("kernel.command.completed")
                      || metric.name == validId<MetricName>("kernel.command.duration_ms")
                      || metric.name == validId<MetricName>("kernel.task.completed")
                      || metric.name == validId<MetricName>("kernel.task.duration_ms");
              })
          == 4);

    command.requestId = validId<RequestId>("request.async-retry");
    auto replayed = kernel.commands().execute(command);
    REQUIRE(replayed.hasValue());
    CHECK(replayed.value().replayed);
    CHECK(replayed.value().taskId == accepted.value().taskId);
    CHECK(asyncHandler->calls.load() == 1U);
    CHECK(kernel.traces().records().size() == 3U);
    CHECK(kernel.traces().exporterFailures().size() == 3U);
    CHECK(kernel.metrics().exporterFailures().size() == 6U);
    REQUIRE(kernel.shutdown().hasValue());
}

TEST_CASE("Asynchronous read-only commands preserve global execution scope", "[runtime][command][task][scope]")
{
    lasercnc::kernel::AppKernel kernel;
    const auto session = validId<SessionId>("session.async-global");
    const auto capability = validId<CapabilityId>("system.compute");
    REQUIRE(kernel.executionServices()
                .configure(std::make_shared<PassValidator>(), std::make_shared<NullLog>())
                .hasValue());
    REQUIRE(kernel.capabilities().replace(session, std::array {capability}).hasValue());

    std::atomic_bool receivedGlobalContext {false};
    REQUIRE(kernel.taskRegistry()
                .registerHandler(
                    descriptor("task.async-compute"),
                    std::make_shared<LambdaHandler>(
                        [&receivedGlobalContext](
                            const TaskRequest& task,
                            const TaskContext& context) {
                            receivedGlobalContext.store(
                                !task.projectId.has_value()
                                && !task.documentId.has_value()
                                && !context.document.has_value());
                            return Result<Value>::success(Value {"computed"});
                        }))
                .hasValue());
    auto handler = std::make_shared<AsyncPlanHandler>();
    auto commandDescriptor = CommandDescriptor {
        validId<CommandName>("command.async-global"),
        Version {1U, 0U, 0U},
        schema("schema.async-global.arguments"),
        schema("schema.async-global.acceptance"),
        ExecutionMode::Asynchronous,
        SideEffectLevel::ReadOnly,
        capability,
        false,
        true,
        true};
    commandDescriptor.scope = ExecutionScope::Global;
    REQUIRE(kernel.commandRegistry()
                .registerAsyncHandler(commandDescriptor, handler)
                .hasValue());

    auto executor = BsThreadPoolExecutor::create(BsThreadPoolExecutorOptions {1U});
    REQUIRE(executor.hasValue());
    REQUIRE(kernel.configureTaskExecutor(std::move(executor).value()).hasValue());
    REQUIRE(kernel.bootstrap().hasValue());

    auto accepted = kernel.commands().execute(CommandRequest {
        validId<RequestId>("request.async-global"),
        ExecutionContext {session, std::nullopt, std::nullopt},
        validId<CommandName>("command.async-global"),
        Version {1U, 0U, 0U},
        Value {Value::Object {}},
        std::nullopt,
        validId<CorrelationId>("correlation.async-global"),
        validId<TraceId>("trace.async-global")});
    REQUIRE(accepted.hasValue());
    REQUIRE(accepted.value().taskId.has_value());
    auto completed = kernel.tasks().wait(*accepted.value().taskId, 2s);
    REQUIRE(completed.hasValue());
    CHECK(completed.value().state == TaskState::Succeeded);
    CHECK(receivedGlobalContext.load());

    REQUIRE(kernel.shutdown().hasValue());
}
