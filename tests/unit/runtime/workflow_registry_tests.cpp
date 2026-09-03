#include <lasercnc/kernel/app_kernel.hpp>
#include <lasercnc/observability/log_service.hpp>
#include <lasercnc/runtime/command_registry.hpp>
#include <lasercnc/runtime/query_registry.hpp>
#include <lasercnc/runtime/workflow_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
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
