#include <lasercnc/kernel/app_kernel.hpp>
#include <lasercnc/observability/log_service.hpp>
#include <lasercnc/platform/task_executor.hpp>
#include <lasercnc/runtime/command_registry.hpp>
#include <lasercnc/runtime/query_registry.hpp>
#include <lasercnc/runtime/workflow_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace lasercnc::foundation;
using namespace lasercnc::kernel;
using namespace lasercnc::observability;
using namespace lasercnc::runtime;

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

class RootKindValidator final : public ISchemaValidator {
public:
    Result<void> validate(const Schema& target, const Value& value) const override
    {
        if(target.rootKind() == SchemaKind::Any
           || (target.rootKind() == SchemaKind::Object && value.kind() == Value::Kind::Object)) {
            return Result<void>::success();
        }
        return Result<void>::failure(makeError(
            "Test.SchemaMismatch", ErrorCategory::Validation, "Schema mismatch"));
    }
};

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

class CommandHandler final : public ICommandHandler {
public:
    Result<Value> execute(const CommandRequest&, ApplicationTransaction&) override
    {
        return Result<Value>::success(Value {Value::Object {}});
    }
};

class QueryHandler final : public IQueryHandler {
public:
    Result<Value> execute(const QueryRequest&, const QueryContext&) override
    {
        return Result<Value>::success(Value {Value::Object {}});
    }
};

class StatefulCommandHandler final : public ICommandHandler {
public:
    Result<Value> execute(
        const CommandRequest& request,
        ApplicationTransaction& transaction) override
    {
        const auto* arguments = request.arguments.getIf<Value::Object>();
        const auto labelEntry = arguments == nullptr ? Value::Object::const_iterator {}
                                                     : arguments->find("label");
        const auto* label = arguments == nullptr || labelEntry == arguments->end()
            ? nullptr
            : labelEntry->second.getIf<std::string>();
        if(label == nullptr) {
            return Result<Value>::failure(makeError(
                "Test.LabelMissing", ErrorCategory::Validation, "A label is required"));
        }
        {
            std::lock_guard lock(mutex_);
            calls_.push_back(*label);
            const auto failure = failures_.find(*label);
            if(failure != failures_.end() && failure->second > 0U) {
                --failure->second;
                return Result<Value>::failure(makeError(
                    failureCodes_.at(*label),
                    ErrorCategory::Infrastructure,
                    "Injected workflow command failure"));
            }
        }
        auto objectId = ObjectId::create("object:" + std::string(request.requestId.value()));
        if(!objectId) {
            return Result<Value>::failure(std::move(objectId).error());
        }
        auto created = transaction.createObject(lasercnc::state::ObjectRecord {
            std::move(objectId).value(),
            validId<ObjectTypeId>("workflow.test.object"),
            request.arguments});
        if(!created) {
            return Result<Value>::failure(std::move(created).error());
        }
        return Result<Value>::success(request.arguments);
    }

    void fail(std::string label, std::uint32_t times, std::string code)
    {
        std::lock_guard lock(mutex_);
        failures_[label] = times;
        failureCodes_[std::move(label)] = std::move(code);
    }

    std::vector<std::string> calls() const
    {
        std::lock_guard lock(mutex_);
        return calls_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> calls_;
    std::map<std::string, std::uint32_t, std::less<>> failures_;
    std::map<std::string, std::string, std::less<>> failureCodes_;
};

class EchoQueryHandler final : public IQueryHandler {
public:
    Result<Value> execute(const QueryRequest& request, const QueryContext&) override
    {
        return Result<Value>::success(request.arguments);
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
        if(stopped_) {
            return Result<void>::failure(makeError(
                "Test.ExecutorStopped", ErrorCategory::Conflict, "Executor stopped"));
        }
        queue_.emplace_back(std::move(work), std::move(completion));
        return Result<void>::success();
    }

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

QueryDescriptor queryDescriptor(const char* name)
{
    return QueryDescriptor {
        validId<QueryName>(name),
        Version {1U, 0U, 0U},
        schema("schema.workflow.query.arguments"),
        schema("schema.workflow.query.result"),
        validId<CapabilityId>("document.read"),
        true,
        true};
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

WorkflowStep commandStep(
    const char* id,
    const char* command,
    std::vector<WorkflowStepId> dependencies = {})
{
    return WorkflowStep {
        validId<WorkflowStepId>(id),
        WorkflowStepKind::Command,
        std::move(dependencies),
        std::nullopt,
        WorkflowCommandCall {
            validId<CommandName>(command),
            Version {1U, 0U, 0U},
            Value {Value::Object {}}},
        std::nullopt,
        Value {},
        {},
        "result",
        std::nullopt,
        WorkflowRetryPolicy {},
        std::nullopt};
}

WorkflowStep queryStep(
    const char* id,
    const char* query,
    std::vector<WorkflowStepId> dependencies = {})
{
    return WorkflowStep {
        validId<WorkflowStepId>(id),
        WorkflowStepKind::Query,
        std::move(dependencies),
        std::nullopt,
        std::nullopt,
        WorkflowQueryCall {
            validId<QueryName>(query),
            Version {1U, 0U, 0U},
            Value {Value::Object {}}},
        Value {},
        {},
        "queryResult",
        std::nullopt,
        WorkflowRetryPolicy {},
        std::nullopt};
}

WorkflowStep barrierStep(const char* id, std::vector<WorkflowStepId> dependencies)
{
    return WorkflowStep {
        validId<WorkflowStepId>(id),
        WorkflowStepKind::Barrier,
        std::move(dependencies),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        Value {},
        {},
        {},
        std::nullopt,
        WorkflowRetryPolicy {},
        std::nullopt};
}

WorkflowStep assignStep(
    const char* id,
    const char* binding,
    Value value,
    std::vector<WorkflowStepId> dependencies = {})
{
    return WorkflowStep {
        validId<WorkflowStepId>(id),
        WorkflowStepKind::Assign,
        std::move(dependencies),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::move(value),
        {},
        binding,
        std::nullopt,
        WorkflowRetryPolicy {},
        std::nullopt};
}

WorkflowStep waitStep(
    const char* id,
    const char* taskIdPath,
    const char* resultBinding,
    std::vector<WorkflowStepId> dependencies)
{
    return WorkflowStep {
        validId<WorkflowStepId>(id),
        WorkflowStepKind::WaitTask,
        std::move(dependencies),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        Value {},
        taskIdPath,
        resultBinding,
        std::nullopt,
        WorkflowRetryPolicy {},
        std::nullopt};
}

WorkflowDefinition workflowDefinition(std::vector<WorkflowStep> steps)
{
    return WorkflowDefinition {
        WorkflowDescriptor {
            validId<WorkflowName>("workflow.registry.test"),
            Version {1U, 0U, 0U},
            schema("schema.workflow.input"),
            schema("schema.workflow.result", SchemaKind::Any)},
        std::move(steps),
        Value {Value::Object {}}};
}

void configureServices(AppKernel& kernel)
{
    REQUIRE(kernel.executionServices()
                .configure(
                    std::make_shared<RootKindValidator>(),
                    std::make_shared<NullLogService>())
                .hasValue());
}

WorkflowRequest workflowRequest(const char* id)
{
    return WorkflowRequest {
        validId<WorkflowId>(id),
        validId<WorkflowName>("workflow.registry.test"),
        Value {Value::Object {}},
        validId<SessionId>("session.workflow"),
        validId<ProjectId>("project.workflow"),
        validId<DocumentId>("document.workflow"),
        validId<CorrelationId>("correlation.workflow"),
        validId<TraceId>("trace.workflow"),
        std::nullopt,
        std::nullopt};
}

struct WorkflowRuntimeFixture final {
    WorkflowRuntimeFixture()
        : command(std::make_shared<StatefulCommandHandler>()),
          query(std::make_shared<EchoQueryHandler>())
    {
        configureServices(kernel);
        const auto request = workflowRequest("workflow.fixture");
        REQUIRE(kernel.addDocument(request.projectId, request.documentId).hasValue());
        REQUIRE(kernel.capabilities()
                    .replace(
                        request.sessionId,
                        std::array {
                            validId<CapabilityId>("document.read"),
                            validId<CapabilityId>("document.write")})
                    .hasValue());
        REQUIRE(kernel.commandRegistry()
                    .registerHandler(commandDescriptor("command.action"), command)
                    .hasValue());
        REQUIRE(kernel.queryRegistry()
                    .registerHandler(queryDescriptor("query.echo"), query)
                    .hasValue());
    }

    void bootstrap(WorkflowDefinition definition)
    {
        REQUIRE(kernel.workflowRegistry()
                    .registerDefinition(std::move(definition))
                    .hasValue());
        REQUIRE(kernel.bootstrap().hasValue());
    }

    ManualExecutor& enableAsync()
    {
        auto executor = std::make_unique<ManualExecutor>();
        auto* result = executor.get();
        REQUIRE(kernel.configureTaskExecutor(std::move(executor)).hasValue());
        REQUIRE(kernel.taskRegistry()
                    .registerHandler(
                        taskDescriptor("task.workflow.echo"),
                        std::make_shared<EchoTaskHandler>())
                    .hasValue());
        REQUIRE(kernel.commandRegistry()
                    .registerAsyncHandler(
                        asyncCommandDescriptor("command.async"),
                        std::make_shared<AsyncPlanHandler>())
                    .hasValue());
        return *result;
    }

    ~WorkflowRuntimeFixture()
    {
        if(kernel.state() == AppKernelState::Ready) {
            static_cast<void>(kernel.shutdown());
        }
    }

    AppKernel kernel;
    std::shared_ptr<StatefulCommandHandler> command;
    std::shared_ptr<EchoQueryHandler> query;
};

} // namespace

TEST_CASE("WorkflowRegistry validates stable acyclic definitions", "[workflow][registry]")
{
    CommandRegistry commands;
    QueryRegistry queries;
    WorkflowRegistry workflows(commands, queries);

    const auto first = validId<WorkflowStepId>("step.command");
    REQUIRE(workflows
                .registerDefinition(workflowDefinition({
                    commandStep("step.command", "command.create"),
                    queryStep("step.query", "query.read", {first}),
                    barrierStep(
                        "step.finish", {validId<WorkflowStepId>("step.query")}),
                }))
                .hasValue());
    CHECK(workflows.size() == 1U);
    REQUIRE(workflows.descriptor(validId<WorkflowName>("workflow.registry.test")).hasValue());

    auto duplicate = workflows.registerDefinition(workflowDefinition({}));
    REQUIRE_FALSE(duplicate.hasValue());
    CHECK(std::string(duplicate.error().code.value()) == "Workflow.AlreadyRegistered");
}

TEST_CASE("WorkflowRegistry rejects dependency errors and invalid step shapes", "[workflow][registry]")
{
    CommandRegistry commands;
    QueryRegistry queries;
    WorkflowRegistry workflows(commands, queries);

    auto missing = workflows.registerDefinition(workflowDefinition({barrierStep(
        "step.finish", {validId<WorkflowStepId>("step.missing")})}));
    REQUIRE_FALSE(missing.hasValue());
    CHECK(std::string(missing.error().code.value()) == "Workflow.DependencyNotFound");

    const auto a = validId<WorkflowStepId>("step.a");
    const auto b = validId<WorkflowStepId>("step.b");
    auto cycle = workflows.registerDefinition(workflowDefinition({
        barrierStep("step.a", {b}),
        barrierStep("step.b", {a}),
    }));
    REQUIRE_FALSE(cycle.hasValue());
    CHECK(std::string(cycle.error().code.value()) == "Workflow.DependencyCycle");

    auto invalidRetry = barrierStep("step.retry", {});
    invalidRetry.retry.maxAttempts = 2U;
    auto retry = workflows.registerDefinition(workflowDefinition({std::move(invalidRetry)}));
    REQUIRE_FALSE(retry.hasValue());
    CHECK(std::string(retry.error().code.value()) == "Workflow.RetryUnsupported");
}

TEST_CASE("AppKernel freezes only workflows with exact idempotent operations", "[workflow][kernel]")
{
    AppKernel kernel;
    configureServices(kernel);
    REQUIRE(kernel.commandRegistry()
                .registerHandler(
                    commandDescriptor("command.create"),
                    std::make_shared<CommandHandler>())
                .hasValue());
    REQUIRE(kernel.queryRegistry()
                .registerHandler(
                    queryDescriptor("query.read"), std::make_shared<QueryHandler>())
                .hasValue());
    REQUIRE(kernel.workflowRegistry()
                .registerDefinition(workflowDefinition({
                    commandStep("step.command", "command.create"),
                    queryStep("step.query", "query.read"),
                }))
                .hasValue());

    REQUIRE(kernel.bootstrap().hasValue());
    CHECK(kernel.workflowRegistry().frozen());
    auto closed = kernel.workflowRegistry().registerDefinition(workflowDefinition({}));
    REQUIRE_FALSE(closed.hasValue());
    CHECK(std::string(closed.error().code.value()) == "Workflow.RegistryFrozen");
    REQUIRE(kernel.shutdown().hasValue());
}

TEST_CASE("AppKernel rejects workflow operation drift before Ready", "[workflow][kernel]")
{
    AppKernel kernel;
    configureServices(kernel);
    REQUIRE(kernel.commandRegistry()
                .registerHandler(
                    commandDescriptor("command.non-idempotent", false),
                    std::make_shared<CommandHandler>())
                .hasValue());
    REQUIRE(kernel.workflowRegistry()
                .registerDefinition(workflowDefinition(
                    {commandStep("step.command", "command.non-idempotent")}))
                .hasValue());

    auto bootstrapped = kernel.bootstrap();
    REQUIRE_FALSE(bootstrapped.hasValue());
    CHECK(std::string(bootstrapped.error().code.value())
          == "Workflow.RegistryValidationFailed");
    REQUIRE(bootstrapped.error().cause != nullptr);
    CHECK(std::string(bootstrapped.error().cause->code.value())
          == "Workflow.CommandNotIdempotent");
    CHECK(kernel.state() == AppKernelState::Failed);
}

TEST_CASE("WorkflowRuntime binds variables and skips false branches", "[workflow][runtime]")
{
    WorkflowRuntimeFixture fixture;
    auto condition = WorkflowPredicate {
        WorkflowPredicateKind::IsTrue,
        "enabled",
        Value {}};
    auto command = commandStep(
        "step.command",
        "command.action",
        {validId<WorkflowStepId>("step.enabled"), validId<WorkflowStepId>("step.payload")});
    command.condition = condition;
    command.command->argumentsTemplate = Value {Value::Object {
        {"label", Value {"create"}},
        {"source", Value {Value::Object {{"$ref", Value {"payload"}}}}},
    }};
    command.resultBinding = "commandResult";
    auto skipped = queryStep(
        "step.skipped", "query.echo", {validId<WorkflowStepId>("step.enabled")});
    skipped.condition = WorkflowPredicate {
        WorkflowPredicateKind::IsTrue,
        "disabled",
        Value {}};
    skipped.query->argumentsTemplate = Value {Value::Object {{"unused", Value {true}}}};
    auto finish = barrierStep(
        "step.finish",
        {validId<WorkflowStepId>("step.command"),
         validId<WorkflowStepId>("step.skipped")});
    auto definition = workflowDefinition({
        assignStep("step.enabled", "enabled", Value {true}),
        assignStep("step.disabled", "disabled", Value {false}),
        assignStep("step.payload", "payload", Value {"bound-value"}),
        std::move(command),
        std::move(skipped),
        std::move(finish),
    });
    definition.resultTemplate = Value {Value::Object {
        {"command", Value {Value::Object {{"$ref", Value {"commandResult"}}}}},
    }};
    fixture.bootstrap(std::move(definition));

    auto request = workflowRequest("workflow.variables");
    REQUIRE(fixture.kernel.workflows().startWorkflow(request).hasValue());
    auto result = fixture.kernel.workflows().advance(request.workflowId);
    REQUIRE(result.hasValue());
    INFO(
        "workflow error: "
        << (result.value().error.has_value()
                ? std::string(result.value().error->code.value())
                : std::string("none")));
    std::string stepStates;
    for(const auto& step : result.value().steps) {
        stepStates += std::string(step.stepId.value()) + "="
            + std::to_string(static_cast<int>(step.state)) + ";";
    }
    INFO("workflow steps: " << stepStates);
    CHECK(result.value().state == WorkflowState::Succeeded);
    REQUIRE(result.value().result.has_value());
    const auto* resultObject = result.value().result->getIf<Value::Object>();
    REQUIRE(resultObject != nullptr);
    const auto* commandResult = resultObject->at("command").getIf<Value::Object>();
    REQUIRE(commandResult != nullptr);
    CHECK(*commandResult->at("source").getIf<std::string>() == "bound-value");
    CHECK(fixture.command->calls() == std::vector<std::string> {"create"});
    const auto skippedSnapshot = std::ranges::find_if(
        result.value().steps,
        [](const auto& step) { return step.stepId.value() == "step.skipped"; });
    REQUIRE(skippedSnapshot != result.value().steps.end());
    CHECK(skippedSnapshot->state == WorkflowStepState::Skipped);
}

TEST_CASE("WorkflowRuntime retries only allowlisted command errors", "[workflow][runtime]")
{
    WorkflowRuntimeFixture fixture;
    fixture.command->fail("retry", 1U, "Test.Transient");
    auto command = commandStep("step.retry", "command.action");
    command.command->argumentsTemplate = Value {Value::Object {{"label", Value {"retry"}}}};
    command.retry = WorkflowRetryPolicy {
        2U,
        std::chrono::milliseconds {0},
        {"Test.Transient"}};
    auto definition = workflowDefinition({std::move(command)});
    definition.resultTemplate = Value {Value::Object {}};
    fixture.bootstrap(std::move(definition));

    auto request = workflowRequest("workflow.retry");
    REQUIRE(fixture.kernel.workflows().startWorkflow(request).hasValue());
    auto first = fixture.kernel.workflows().advance(request.workflowId);
    REQUIRE(first.hasValue());
    CHECK(first.value().state == WorkflowState::Waiting);
    REQUIRE(first.value().steps.size() == 1U);
    CHECK(first.value().steps[0].attempt == 1U);

    auto second = fixture.kernel.workflows().advance(request.workflowId);
    REQUIRE(second.hasValue());
    CHECK(second.value().state == WorkflowState::Succeeded);
    CHECK(second.value().steps[0].attempt == 2U);
    CHECK(fixture.command->calls() == std::vector<std::string> {"retry", "retry"});
}

TEST_CASE("WorkflowRuntime compensates successful steps in reverse order", "[workflow][runtime]")
{
    WorkflowRuntimeFixture fixture;
    fixture.command->fail("fail", 1U, "Test.Fatal");

    auto first = commandStep("step.first", "command.action");
    first.command->argumentsTemplate = Value {Value::Object {{"label", Value {"first"}}}};
    first.compensation = WorkflowCompensation {WorkflowCommandCall {
        validId<CommandName>("command.action"),
        Version {1U, 0U, 0U},
        Value {Value::Object {{"label", Value {"undo-first"}}}}}};

    auto second = commandStep(
        "step.second", "command.action", {validId<WorkflowStepId>("step.first")});
    second.command->argumentsTemplate = Value {Value::Object {{"label", Value {"second"}}}};
    second.compensation = WorkflowCompensation {WorkflowCommandCall {
        validId<CommandName>("command.action"),
        Version {1U, 0U, 0U},
        Value {Value::Object {{"label", Value {"undo-second"}}}}}};

    auto failing = commandStep(
        "step.third", "command.action", {validId<WorkflowStepId>("step.second")});
    failing.command->argumentsTemplate = Value {Value::Object {{"label", Value {"fail"}}}};
    fixture.bootstrap(workflowDefinition(
        {std::move(first), std::move(second), std::move(failing)}));

    auto request = workflowRequest("workflow.compensation");
    REQUIRE(fixture.kernel.workflows().startWorkflow(request).hasValue());
    auto result = fixture.kernel.workflows().advance(request.workflowId);
    REQUIRE(result.hasValue());
    CHECK(result.value().state == WorkflowState::Compensated);
    REQUIRE(result.value().error.has_value());
    CHECK(std::string(result.value().error->code.value()) == "Test.Fatal");
    CHECK(fixture.command->calls()
          == std::vector<std::string> {
              "first", "second", "fail", "undo-second", "undo-first"});
}

TEST_CASE("WorkflowRuntime cancellation is explicit and terminal", "[workflow][runtime]")
{
    WorkflowRuntimeFixture fixture;
    fixture.bootstrap(workflowDefinition(
        {assignStep("step.pending", "value", Value {true})}));
    auto request = workflowRequest("workflow.cancel");
    REQUIRE(fixture.kernel.workflows().startWorkflow(request).hasValue());

    auto cancelled = fixture.kernel.workflows().cancel(request.workflowId);
    REQUIRE(cancelled.hasValue());
    CHECK(cancelled.value().state == WorkflowState::Cancelled);
    REQUIRE(cancelled.value().error.has_value());
    CHECK(std::string(cancelled.value().error->code.value()) == "Workflow.Cancelled");
    CHECK(cancelled.value().steps[0].state == WorkflowStepState::Cancelled);
}

TEST_CASE("WorkflowRuntime launches independent tasks and resumes wait steps", "[workflow][runtime][task]")
{
    WorkflowRuntimeFixture fixture;
    auto& executor = fixture.enableAsync();

    auto first = commandStep("step.async-a", "command.async");
    first.command->argumentsTemplate = Value {Value::Object {{"label", Value {"a"}}}};
    first.resultBinding = "acceptance.a";
    first.taskIdBinding = "tasks.a";
    auto second = commandStep("step.async-b", "command.async");
    second.command->argumentsTemplate = Value {Value::Object {{"label", Value {"b"}}}};
    second.resultBinding = "acceptance.b";
    second.taskIdBinding = "tasks.b";
    auto waitFirst = waitStep(
        "step.wait-a",
        "tasks.a",
        "outputs.a",
        {validId<WorkflowStepId>("step.async-a")});
    auto waitSecond = waitStep(
        "step.wait-b",
        "tasks.b",
        "outputs.b",
        {validId<WorkflowStepId>("step.async-b")});
    auto definition = workflowDefinition(
        {std::move(first), std::move(second), std::move(waitFirst), std::move(waitSecond)});
    definition.resultTemplate = Value {Value::Object {
        {"a", Value {Value::Object {{"$ref", Value {"outputs.a.label"}}}}},
        {"b", Value {Value::Object {{"$ref", Value {"outputs.b.label"}}}}},
    }};
    fixture.bootstrap(std::move(definition));

    auto request = workflowRequest("workflow.parallel-tasks");
    REQUIRE(fixture.kernel.workflows().startWorkflow(request).hasValue());
    auto waiting = fixture.kernel.workflows().advance(request.workflowId);
    REQUIRE(waiting.hasValue());
    CHECK(waiting.value().state == WorkflowState::Waiting);
    CHECK(executor.queued() == 2U);

    executor.runAll();
    auto completed = fixture.kernel.workflows().advance(request.workflowId);
    REQUIRE(completed.hasValue());
    CHECK(completed.value().state == WorkflowState::Succeeded);
    REQUIRE(completed.value().result.has_value());
    const auto* result = completed.value().result->getIf<Value::Object>();
    REQUIRE(result != nullptr);
    CHECK(*result->at("a").getIf<std::string>() == "a");
    CHECK(*result->at("b").getIf<std::string>() == "b");
}

TEST_CASE("WorkflowRuntime enforces assertions and deadlines", "[workflow][runtime][timeout]")
{
    SECTION("assertion") {
        WorkflowRuntimeFixture fixture;
        auto assertion = WorkflowStep {
            validId<WorkflowStepId>("step.assert"),
            WorkflowStepKind::Assert,
            {validId<WorkflowStepId>("step.value")},
            WorkflowPredicate {
                WorkflowPredicateKind::IsTrue,
                "accepted",
                Value {}},
            std::nullopt,
            std::nullopt,
            Value {},
            {},
            {},
            std::nullopt,
            WorkflowRetryPolicy {},
            std::nullopt};
        fixture.bootstrap(workflowDefinition({
            assignStep("step.value", "accepted", Value {false}),
            std::move(assertion),
        }));
        auto request = workflowRequest("workflow.assertion");
        REQUIRE(fixture.kernel.workflows().startWorkflow(request).hasValue());
        auto result = fixture.kernel.workflows().advance(request.workflowId);
        REQUIRE(result.hasValue());
        CHECK(result.value().state == WorkflowState::Failed);
        REQUIRE(result.value().error.has_value());
        CHECK(std::string(result.value().error->code.value())
              == "Workflow.AssertionFailed");
    }

    SECTION("deadline") {
        WorkflowRuntimeFixture fixture;
        fixture.bootstrap(workflowDefinition(
            {assignStep("step.value", "accepted", Value {true})}));
        auto request = workflowRequest("workflow.deadline");
        request.deadline = std::chrono::system_clock::now() - std::chrono::milliseconds {1};
        REQUIRE(fixture.kernel.workflows().startWorkflow(request).hasValue());
        auto result = fixture.kernel.workflows().advance(request.workflowId);
        REQUIRE(result.hasValue());
        CHECK(result.value().state == WorkflowState::Failed);
        REQUIRE(result.value().error.has_value());
        CHECK(std::string(result.value().error->code.value())
              == "Workflow.DeadlineExceeded");
    }
}

TEST_CASE("WorkflowRuntime exposes compensation failure without erasing the original error", "[workflow][runtime][compensation]")
{
    WorkflowRuntimeFixture fixture;
    fixture.command->fail("fail", 1U, "Test.OriginalFailure");
    fixture.command->fail("undo", 1U, "Test.CompensationFailure");

    auto successful = commandStep("step.success", "command.action");
    successful.command->argumentsTemplate = Value {Value::Object {{"label", Value {"success"}}}};
    successful.compensation = WorkflowCompensation {WorkflowCommandCall {
        validId<CommandName>("command.action"),
        Version {1U, 0U, 0U},
        Value {Value::Object {{"label", Value {"undo"}}}}}};
    auto failing = commandStep(
        "step.failure", "command.action", {validId<WorkflowStepId>("step.success")});
    failing.command->argumentsTemplate = Value {Value::Object {{"label", Value {"fail"}}}};
    fixture.bootstrap(workflowDefinition({std::move(successful), std::move(failing)}));

    auto request = workflowRequest("workflow.compensation-failure");
    REQUIRE(fixture.kernel.workflows().startWorkflow(request).hasValue());
    auto result = fixture.kernel.workflows().advance(request.workflowId);
    REQUIRE(result.hasValue());
    CHECK(result.value().state == WorkflowState::CompensationFailed);
    REQUIRE(result.value().error.has_value());
    CHECK(std::string(result.value().error->code.value()) == "Test.OriginalFailure");
    REQUIRE(result.value().compensationErrors.size() == 1U);
    CHECK(std::string(result.value().compensationErrors[0].code.value())
          == "Test.CompensationFailure");
}
