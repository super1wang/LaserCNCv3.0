#include <lasercnc/kernel/app_kernel.hpp>
#include <lasercnc/observability/log_service.hpp>
#include <lasercnc/platform/task_executor.hpp>
#include <catch2/catch_test_macros.hpp>
#include "kernel_test_module.hpp"
#include "persistence_fixture.hpp"
#include "fault_injecting_backend.hpp"
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

using namespace lasercnc::foundation;
using namespace lasercnc::kernel;
using namespace lasercnc::runtime;
using namespace lasercnc::observability;

namespace {
template <typename Id>
Id validId(const char* value)
{
    auto created = Id::create(value);
    if(!created) {
        throw std::logic_error("Invalid test id");
    }
    return std::move(created).value();
}

Schema schema(const char* id, SchemaKind kind = SchemaKind::Object)
{
    auto created = Schema::create(validId<SchemaId>(id), Version {1U, 0U, 0U}, kind);
    if(!created) {
        throw std::logic_error("Invalid test schema");
    }
    return std::move(created).value();
}

class NullLogService final : public ILogService {
public:
    Result<void> write(const LogRecord&) override
    {
        return Result<void>::success();
    }

    Result<void> flush() override
    {
        return Result<void>::success();
    }
};

class EchoTaskHandler final : public ITaskHandler {
public:
    Result<Value> execute(const TaskRequest& request, const TaskContext&) override
    {
        return Result<Value>::success(request.input);
    }
};

class AsyncPlanHandler final : public IAsyncCommandHandler {
public:
    Result<AsyncCommandPlan> prepare(const CommandRequest& request) override
    {
        auto taskId = TaskId::create("task:" + std::string(request.requestId.value()));
        if(!taskId) {
            return Result<AsyncCommandPlan>::failure(std::move(taskId).error());
        }
        return Result<AsyncCommandPlan>::success(AsyncCommandPlan {
            TaskRequest {
                std::move(taskId).value(),
                validId<TaskName>("task.workflow.echo"),
                request.arguments,
                request.traceId},
            Value {Value::Object {{"accepted", Value {true}}}}});
    }
};

class ManualExecutor final : public lasercnc::platform::ITaskExecutor {
public:
    Result<void> submit(
        lasercnc::platform::ExecutorWork work,
        lasercnc::platform::ExecutorCompletion completion) override
    {
        if(beforeSubmit) { auto callback = std::exchange(beforeSubmit, {}); callback(); }
        if(stopped_) {
            return Result<void>::failure(makeError(
                "Test.ExecutorStopped", ErrorCategory::Conflict, "Executor stopped"));
        }
        queue_.emplace_back(std::move(work), std::move(completion));
        return Result<void>::success();
    }

    std::function<void()> beforeSubmit;

    Result<void> waitIdle() override
    {
        runAll();
        return Result<void>::success();
    }

    Result<void> shutdown() override
    {
        runAll();
        stopped_ = true;
        return Result<void>::success();
    }

    std::size_t concurrency() const noexcept override
    {
        return 2U;
    }

    std::size_t queued() const noexcept
    {
        return queue_.size();
    }

    void runAll()
    {
        while(!queue_.empty()) {
            auto item = std::move(queue_.front());
            queue_.pop_front();
            auto result = item.first();
            item.second(std::move(result));
        }
    }

private:
    std::deque<std::pair<
        lasercnc::platform::ExecutorWork,
        lasercnc::platform::ExecutorCompletion>> queue_;
    bool stopped_{false};
};

CommandDescriptor commandDescriptor(const char* name, bool idempotent = true)
{
    return CommandDescriptor {
        validId<CommandName>(name),
        Version {1U, 0U, 0U},
        schema("schema.workflow.command.arguments"),
        schema("schema.workflow.command.result"),
        ExecutionMode::Synchronous,
        SideEffectLevel::DocumentWrite,
        validId<CapabilityId>("document.write"),
        false,
        true,
        idempotent};
}

CommandDescriptor asyncCommandDescriptor(const char* name)
{
    auto descriptor = commandDescriptor(name);
    descriptor.executionMode = ExecutionMode::Asynchronous;
    descriptor.sideEffect = SideEffectLevel::ReadOnly;
    return descriptor;
}

TaskDescriptor taskDescriptor(const char* name)
{
    return TaskDescriptor {
        validId<TaskName>(name),
        Version {1U, 0U, 0U},
        schema("schema.workflow.task.input"),
        schema("schema.workflow.task.result")};
}


}

TEST_CASE("Project only task acceptance rollback and terminal persistence retain ownership", "[task][project-admission][persistence]")
{
    using namespace lasercnc::infrastructure;
    using namespace lasercnc::test;
    for(const bool throws : {false, true}) {
        AppKernel kernel;
        REQUIRE(kernel.executionServices().configure(std::make_shared<JsonconsAdapter>(),
            std::make_shared<NullLogService>()));
        auto ownedExecutor = std::make_unique<ManualExecutor>();
        auto& executor = *ownedExecutor;
        REQUIRE(kernel.configureTaskExecutor(std::move(ownedExecutor)));
        REQUIRE(registerTask(kernel, taskDescriptor("task.workflow.echo"), std::make_shared<EchoTaskHandler>()));
        REQUIRE(kernel.capabilities().replace(validId<SessionId>("session.workflow"),
            std::array{validId<CapabilityId>("document.write")}));

        auto sqlite = SqlitePersistenceBackend::open({":memory:"});
        REQUIRE(sqlite);
        auto backend = std::make_unique<FaultInjectingBackend>(std::move(sqlite).value());
        auto* faults = backend.get();
        REQUIRE(kernel.configurePersistence(std::move(backend),
            std::make_shared<JsonconsAdapter>(), std::make_shared<Sha256HashService>()));
        const auto project = validId<ProjectId>("project.task-fault");
        REQUIRE(kernel.addProject(project));
        auto descriptor = asyncCommandDescriptor("command.project-fault");
        descriptor.scope = ExecutionScope::Project;
        REQUIRE(registerAsyncCommand(kernel, descriptor, std::make_shared<AsyncPlanHandler>()));
        REQUIRE(kernel.bootstrap());
        const auto request = CommandRequest{validId<RequestId>("request.project-fault"),
            {validId<SessionId>("session.workflow"), project, std::nullopt}, validId<CommandName>("command.project-fault"),
            {1U, 0U, 0U}, Value{Value::Object{}}, std::nullopt,
            validId<CorrelationId>("correlation.project-fault"), validId<TraceId>("trace.project-fault")};
        const auto task = validId<TaskId>("task:request.project-fault");
        unsigned insertProbes = 0U;
        unsigned terminalProbes = 0U;
        faults->beforeOperation = [&](BackendPoint point, std::string_view sql) {
            if(point != BackendPoint::Execute) { return; }
            const bool insert = sql.find("INSERT INTO task_history") != std::string_view::npos;
            const bool terminal = sql.find("UPDATE task_history SET status=") != std::string_view::npos;
            if(!insert && !terminal) { return; }
            if(insert) { ++insertProbes; } else { ++terminalProbes; }
            auto closed = kernel.projectRuntime().close(project);
            CHECK_FALSE(closed);
            if(!closed) { CHECK(std::string(closed.error().code.value()) == "Project.CloseBlocked"); }
            CHECK(kernel.scheduler().activeTaskCount(project) == 1U);
        };
        faults->arm(BackendPoint::Execute, "INSERT INTO task_history", 1U, throws);
        CHECK_FALSE(kernel.execution().executeCommand(request));
        CHECK(faults->hits == 1U);
        CHECK(executor.queued() == 0U);
        CHECK(kernel.scheduler().activeTaskCount(project) == 0U);
        CHECK(kernel.projectRuntime().lifecycle(project).value().activities == 0U);
        auto absent = kernel.persistence().taskHistory(task);
        REQUIRE(absent);
        CHECK_FALSE(absent.value().has_value());
        REQUIRE(kernel.projectRuntime().close(project));
        REQUIRE(kernel.projectRuntime().open(project));
        REQUIRE(kernel.execution().executeCommand(request));
        REQUIRE(executor.queued() == 1U);
        executor.runAll();
        CHECK(insertProbes == 2U);
        CHECK(terminalProbes == 1U);
        CHECK(kernel.scheduler().activeTaskCount(project) == 0U);
        auto persisted = kernel.persistence().taskHistory(task);
        REQUIRE(persisted);
        REQUIRE(persisted.value().has_value());
        CHECK(persisted.value()->state == TaskState::Succeeded);
        faults->beforeOperation = {};
        REQUIRE(kernel.projectRuntime().close(project));
        REQUIRE(kernel.shutdown());
    }
}

TEST_CASE("Lifecycle commands cannot close a project while its long task is pending or publishing", "[task][project-admission][lifecycle-control]")
{
    using namespace lasercnc::infrastructure;
    using namespace lasercnc::test;
    AppKernel kernel;
    REQUIRE(kernel.executionServices().configure(std::make_shared<JsonconsAdapter>(), std::make_shared<NullLogService>()));
    auto owned = std::make_unique<ManualExecutor>();
    auto& executor = *owned;
    REQUIRE(kernel.configureTaskExecutor(std::move(owned)));
    REQUIRE(registerTask(kernel, taskDescriptor("task.workflow.echo"), std::make_shared<EchoTaskHandler>()));
    const auto session = validId<SessionId>("session.lifecycle");
    const auto project = validId<ProjectId>("project.lifecycle.task");
    REQUIRE(kernel.capabilities().replace(session, std::array{validId<CapabilityId>("document.write")}));
    REQUIRE(kernel.addProject(project));
    auto submission = asyncCommandDescriptor("command.lifecycle.submit");
    submission.scope = ExecutionScope::Project;
    REQUIRE(registerAsyncCommand(kernel, submission, std::make_shared<AsyncPlanHandler>()));
    auto close = commandDescriptor("command.lifecycle.close", false);
    close.scope = ExecutionScope::Project;
    close.sideEffect = SideEffectLevel::LifecycleControl;
    close.lifecycleOperation = LifecycleOperation::ProjectClose;
    REQUIRE(installKernelTestModule(kernel, [&](auto& builder) { builder.lifecycleCommand(close); }));
    auto sqlite = SqlitePersistenceBackend::open({":memory:"});
    REQUIRE(sqlite);
    auto backend = std::make_unique<FaultInjectingBackend>(std::move(sqlite).value());
    auto* faults = backend.get();
    REQUIRE(kernel.configurePersistence(std::move(backend), std::make_shared<JsonconsAdapter>(), std::make_shared<Sha256HashService>()));
    REQUIRE(kernel.bootstrap());
    auto request = CommandRequest{validId<RequestId>("request.lifecycle.task"), {session, project, std::nullopt},
        submission.name, submission.version, Value{Value::Object{}}, std::nullopt,
        validId<CorrelationId>("correlation.lifecycle.task"), validId<TraceId>("trace.lifecycle.task")};
    auto closing = request;
    closing.command = close.name;
    const auto blocked = [&] {
        auto result = kernel.execution().executeCommand(closing);
        REQUIRE_FALSE(result);
        CHECK(std::string(result.error().code.value()) == "Project.CloseBlocked");
        CHECK(kernel.projectRuntime().lifecycle(project).value().state == ProjectLifecycleState::Open);
    };
    REQUIRE(kernel.execution().executeCommand(request));
    blocked();
    unsigned int terminalProbes = 0U;
    faults->beforeOperation = [&](BackendPoint point, std::string_view sql) {
        if(point == BackendPoint::Execute && sql.find("UPDATE task_history SET status=") != std::string_view::npos) {
            ++terminalProbes;
            blocked();
        }
    };
    executor.runAll();
    faults->beforeOperation = {};
    CHECK(terminalProbes == 1U);
    REQUIRE(kernel.execution().executeCommand(closing));
    REQUIRE(kernel.shutdown());
}
