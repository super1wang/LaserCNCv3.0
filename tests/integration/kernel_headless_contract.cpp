#include <lasercnc/infrastructure/jsoncons_adapter.hpp>
#include <lasercnc/infrastructure/bs_thread_pool_executor.hpp>
#include <lasercnc/infrastructure/filesystem_snapshot_store.hpp>
#include <lasercnc/infrastructure/sha256_hash_service.hpp>
#include <lasercnc/infrastructure/spdlog_log_service.hpp>
#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>
#include <lasercnc/kernel/app_kernel.hpp>
#include <lasercnc/observability/log_observability_exporter.hpp>
#include "kernel_test_module.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

using namespace lasercnc::foundation;
using namespace lasercnc::infrastructure;
using namespace lasercnc::kernel;
using namespace lasercnc::messaging;
using namespace lasercnc::observability;
using namespace lasercnc::runtime;
using namespace lasercnc::state;

namespace {

template <typename Id>
Result<Id> makeId(const char* value)
{
    return Id::create(value);
}

Result<Schema> makeObjectSchema(const char* id, bool requiresData)
{
    Value::Object stringProperty {{"type", Value {"string"}}};
    Value::Object properties {{"id", Value {stringProperty}}};
    Value::Array required {Value {"id"}};
    if(requiresData) {
        properties.emplace("data", Value {stringProperty});
        required.push_back(Value {"data"});
    }
    Value::Object constraints {
        {"additionalProperties", Value {false}},
        {"properties", Value {std::move(properties)}},
        {"required", Value {std::move(required)}},
    };
    auto schemaId = makeId<SchemaId>(id);
    if(!schemaId.hasValue()) {
        return Result<Schema>::failure(std::move(schemaId).error());
    }
    return Schema::create(
        std::move(schemaId).value(), Version {1U, 0U, 0U}, SchemaKind::Object,
        Value {std::move(constraints)});
}

template <typename Id>
Id requiredId(const char* value)
{
    auto id = makeId<Id>(value);
    if(!id.hasValue()) {
        throw std::logic_error("Static integration ID is invalid");
    }
    return std::move(id).value();
}

class PutObjectHandler final : public ICommandHandler {
public:
    Result<Value> execute(const CommandRequest& request, ApplicationTransaction& transaction) override
    {
        ++calls;
        const auto& arguments = *request.arguments.getIf<Value::Object>();
        const auto idText = *arguments.at("id").getIf<std::string>();
        const auto data = *arguments.at("data").getIf<std::string>();
        auto objectId = ObjectId::create(idText);
        if(!objectId.hasValue()) {
            return Result<Value>::failure(std::move(objectId).error());
        }
        const auto stableId = objectId.value();
        auto created = transaction.createObject(ObjectRecord {
            std::move(objectId).value(),
            requiredId<ObjectTypeId>("kernel.contract.object"),
            Value {data}});
        if(!created.hasValue()) {
            return Result<Value>::failure(std::move(created).error());
        }
        auto collected = transaction.collectEvent(PendingDomainEvent {
            requiredId<EventName>("kernel.contract.object-created"),
            Version {1U, 0U, 0U},
            stableId,
            Value {Value::Object {{"id", Value {idText}}}}});
        if(!collected.hasValue()) {
            return Result<Value>::failure(std::move(collected).error());
        }
        return Result<Value>::success(Value {Value::Object {{"id", Value {idText}}}});
    }

    std::atomic_size_t calls{0U};
};

class GetObjectHandler final : public IQueryHandler {
public:
    Result<Value> execute(const QueryRequest& request, const QueryContext& context) override
    {
        if(!context.document.has_value()) {
            return Result<Value>::failure(makeError(
                "Contract.DocumentMissing", ErrorCategory::Internal,
                "The immutable document snapshot is missing"));
        }
        const auto& arguments = *request.arguments.getIf<Value::Object>();
        const auto idText = *arguments.at("id").getIf<std::string>();
        auto objectId = ObjectId::create(idText);
        if(!objectId.hasValue()) {
            return Result<Value>::failure(std::move(objectId).error());
        }
        const auto* object = context.document->objects().find(objectId.value());
        if(object == nullptr) {
            return Result<Value>::failure(makeError(
                "Contract.ObjectMissing", ErrorCategory::NotFound,
                "The contract object is missing"));
        }
        return Result<Value>::success(Value {Value::Object {
            {"data", object->data},
            {"id", Value {idText}},
        }});
    }
};

class ContractTaskHandler final : public ITaskHandler {
public:
    Result<Value> execute(const TaskRequest& request, const TaskContext& context) override
    {
        if(!context.document.has_value()
           || request.correlationId
               != requiredId<CorrelationId>("correlation.cli.task")
           || context.traceId != requiredId<TraceId>("trace.cli.task")) {
            return Result<Value>::failure(makeError(
                "Contract.TaskContextInvalid",
                ErrorCategory::Internal,
                "The asynchronous command context was not propagated"));
        }
        auto progressed = context.progress.report(0.5, "computing");
        if(!progressed) {
            return Result<Value>::failure(std::move(progressed).error());
        }
        const auto& arguments = *request.input.getIf<Value::Object>();
        return Result<Value>::success(Value {Value::Object {
            {"data", Value {"task-verified"}},
            {"id", arguments.at("id")},
        }});
    }
};

class ContractAsyncCommandHandler final : public IAsyncCommandHandler {
public:
    Result<AsyncCommandPlan> prepare(const CommandRequest& request) override
    {
        return Result<AsyncCommandPlan>::success(AsyncCommandPlan {
            TaskRequest {
                requiredId<TaskId>("task.cli.contract"),
                requiredId<TaskName>("kernel.contract.compute"),
                request.arguments,
                requiredId<TraceId>("trace.must-be-overridden")},
            Value {Value::Object {{"id", Value {"task.cli.contract"}}}}});
    }
};

class QuietLogService final : public ILogService {
public:
    Result<void> write(const LogRecord&) override { return Result<void>::success(); }
    Result<void> flush() override { return Result<void>::success(); }
};

class ContractDiagnosticCheck final : public IDiagnosticCheck {
public:
    Result<DiagnosticReport> run() override
    {
        return Result<DiagnosticReport>::success(DiagnosticReport {
            requiredId<DiagnosticId>("kernel.contract.persistence"),
            DiagnosticStatus::Healthy,
            "persistent",
            Value {Value::Object {{"mode", Value {"headless"}}}},
            {}});
    }
};

std::filesystem::path uniqueLogPath()
{
    static std::atomic_ullong sequence {0U};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / ("lasercnc-kernel-headless-" + std::to_string(tick) + '-'
           + std::to_string(sequence.fetch_add(1U)) + ".jsonl");
}

int fail(const char* stage, const Error& error)
{
    std::cerr << stage << ": " << error.code.value() << ' ' << error.message << '\n';
    return 1;
}

Result<void> configureObservability(
    lasercnc::kernel::AppKernel& kernel,
    const std::shared_ptr<ILogService>& logService)
{
    auto created = LogObservabilityExporter::create(logService);
    if(!created) {
        return Result<void>::failure(std::move(created).error());
    }
    auto exporter = std::move(created).value();
    auto trace = kernel.traces().addExporter(exporter);
    if(!trace) {
        return trace;
    }
    return kernel.metrics().addExporter(std::move(exporter));
}

bool containsObservabilityJsonl(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    const std::string content {
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return content.find("trace.span") != std::string::npos
        && content.find("metric.observation") != std::string::npos;
}

Schema contractSchema(const char* id, SchemaKind kind)
{
    auto created = Schema::create(
        requiredId<SchemaId>(id), Version {1U, 0U, 0U}, kind);
    if(!created) {
        throw std::logic_error("Static workflow schema is invalid");
    }
    return std::move(created).value();
}

WorkflowDefinition persistenceWorkflowDefinition()
{
    WorkflowStep write {
        requiredId<WorkflowStepId>("step.persistence.write"),
        WorkflowStepKind::Command,
        {},
        std::nullopt,
        WorkflowCommandCall {
            requiredId<CommandName>("kernel.persistence.object.put"),
            Version {1U, 0U, 0U},
            Value {Value::Object {
                {"data", Value {"workflow-recovered"}},
                {"id", Value {"object.persistence.workflow"}},
            }}},
        std::nullopt,
        Value {},
        {},
        "writeResult",
        std::nullopt,
        WorkflowRetryPolicy {},
        std::nullopt,
        {}};
    return WorkflowDefinition {
        WorkflowDescriptor {
            requiredId<WorkflowName>("kernel.persistence.workflow"),
            Version {1U, 0U, 0U},
            contractSchema("schema.persistence.workflow.input", SchemaKind::Object),
            contractSchema("schema.persistence.workflow.result", SchemaKind::Any)},
        {std::move(write)},
        Value {Value::Object {{"$ref", Value {"writeResult.id"}}}}};
}

WorkflowRequest persistenceWorkflowRequest()
{
    return WorkflowRequest {
        requiredId<WorkflowId>("workflow.persistence.interrupted"),
        requiredId<WorkflowName>("kernel.persistence.workflow"),
        Value {Value::Object {}},
        requiredId<SessionId>("session.persistence-contract"),
        requiredId<ProjectId>("project.persistence-contract"),
        requiredId<DocumentId>("document.persistence-contract"),
        requiredId<CorrelationId>("correlation.persistence-workflow"),
        requiredId<TraceId>("trace.persistence-workflow"),
        std::nullopt,
        std::nullopt};
}

Result<void> configurePersistenceContract(
    lasercnc::kernel::AppKernel& kernel,
    const std::filesystem::path& stateRoot,
    const std::shared_ptr<PutObjectHandler>& putHandler)
{
    std::error_code error;
    std::filesystem::create_directories(stateRoot, error);
    if(error) {
        return Result<void>::failure(makeError(
            "Contract.StateDirectoryFailed",
            ErrorCategory::Infrastructure,
            "The persistence contract state directory could not be created"));
    }
    auto validator = std::make_shared<JsonconsAdapter>();
    auto execution = kernel.executionServices().configure(
        validator, std::make_shared<QuietLogService>());
    if(!execution) {
        return execution;
    }
    const auto session = requiredId<SessionId>("session.persistence-contract");
    const std::array capabilities {
        requiredId<CapabilityId>("document.read"),
        requiredId<CapabilityId>("document.write")};
    auto granted = kernel.capabilities().replace(session, capabilities);
    if(!granted) {
        return granted;
    }
    auto putArguments = makeObjectSchema("schema.persistence.put.arguments", true);
    auto putResult = makeObjectSchema("schema.persistence.put.result", false);
    auto getArguments = makeObjectSchema("schema.persistence.get.arguments", false);
    auto getResult = makeObjectSchema("schema.persistence.get.result", true);
    if(!putArguments || !putResult || !getArguments || !getResult) {
        return Result<void>::failure(makeError(
            "Contract.SchemaFailed",
            ErrorCategory::Internal,
            "The persistence contract schemas could not be created"));
    }
    auto command = lasercnc::test::registerCommand(kernel,
        CommandDescriptor {
            requiredId<CommandName>("kernel.persistence.object.put"),
            Version {1U, 0U, 0U},
            std::move(putArguments).value(),
            std::move(putResult).value(),
            ExecutionMode::Synchronous,
            SideEffectLevel::DocumentWrite,
            requiredId<CapabilityId>("document.write"),
            false,
            true,
            true},
        putHandler);
    if(!command) {
        return command;
    }
    auto query = lasercnc::test::registerQuery(kernel,
        QueryDescriptor {
            requiredId<QueryName>("kernel.persistence.object.get"),
            Version {1U, 0U, 0U},
            std::move(getArguments).value(),
            std::move(getResult).value(),
            requiredId<CapabilityId>("document.read"),
            ExecutionScope::Document,
            true},
        std::make_shared<GetObjectHandler>());
    if(!query) {
        return query;
    }
    auto workflow = lasercnc::test::registerWorkflow(kernel,
        persistenceWorkflowDefinition());
    if(!workflow) {
        return workflow;
    }
    auto diagnostic = kernel.diagnostics().registerCheck(
        requiredId<DiagnosticId>("kernel.contract.persistence"),
        std::make_shared<ContractDiagnosticCheck>());
    if(!diagnostic) {
        return diagnostic;
    }
    auto backend = SqlitePersistenceBackend::open(
        SqliteConnectionOptions {stateRoot / "kernel.db"});
    if(!backend) {
        return Result<void>::failure(std::move(backend).error());
    }
    auto snapshots = FilesystemSnapshotStore::create(
        FilesystemSnapshotStoreOptions {stateRoot / "snapshots", 1024U * 1024U});
    if(!snapshots) {
        return Result<void>::failure(std::move(snapshots).error());
    }
    return kernel.persistence().configure(
        std::move(backend).value(),
        validator,
        std::make_shared<Sha256HashService>(),
        std::move(snapshots).value());
}

CommandRequest persistenceCommand(
    const char* requestId,
    const char* objectId,
    const char* data,
    Revision expected,
    const char* idempotencyKey)
{
    return CommandRequest {
        requiredId<RequestId>(requestId),
        ExecutionContext {
            requiredId<SessionId>("session.persistence-contract"),
            requiredId<ProjectId>("project.persistence-contract"),
            requiredId<DocumentId>("document.persistence-contract")},
        requiredId<CommandName>("kernel.persistence.object.put"),
        Version {1U, 0U, 0U},
        Value {Value::Object {
            {"data", Value {data}}, {"id", Value {objectId}}}},
        expected,
        requiredId<CorrelationId>("correlation.persistence-contract"),
        requiredId<TraceId>("trace.persistence-contract"),
        requiredId<IdempotencyKey>(idempotencyKey)};
}

Result<Value> queryPersistentObject(
    lasercnc::kernel::AppKernel& kernel,
    const char* requestId,
    const char* objectId)
{
    auto queried = kernel.execution().executeQuery(QueryRequest {
        requiredId<RequestId>(requestId),
        ExecutionContext {
            requiredId<SessionId>("session.persistence-contract"),
            requiredId<ProjectId>("project.persistence-contract"),
            requiredId<DocumentId>("document.persistence-contract")},
        requiredId<QueryName>("kernel.persistence.object.get"),
        Version {1U, 0U, 0U},
        Value {Value::Object {{"id", Value {objectId}}}},
        requiredId<CorrelationId>("correlation.persistence-contract"),
        requiredId<TraceId>("trace.persistence-contract")});
    if(!queried) {
        return Result<Value>::failure(std::move(queried).error());
    }
    return Result<Value>::success(std::move(queried).value().result);
}

int runRoundTrip()
{
    const auto logPath = uniqueLogPath();
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove(logPath, ignored));

    SpdlogLogOptions options;
    options.enableConsole = false;
    options.jsonlFilePath = logPath;
    auto logger = SpdlogLogService::create(options);
    if(!logger.hasValue()) {
        return fail("logger", logger.error());
    }
    auto validator = std::make_shared<JsonconsAdapter>();
    std::shared_ptr<lasercnc::observability::ILogService> logService(
        std::move(logger).value());

    lasercnc::kernel::AppKernel kernel;
    auto configured = kernel.executionServices().configure(validator, logService);
    if(!configured.hasValue()) {
        return fail("configure", configured.error());
    }
    auto observability = configureObservability(kernel, logService);
    if(!observability) {
        return fail("observability", observability.error());
    }

    const auto project = requiredId<ProjectId>("project.headless-contract");
    const auto document = requiredId<DocumentId>("document.headless-contract");
    const auto session = requiredId<SessionId>("session.cli-contract");
    auto added = kernel.addDocument(project, document);
    if(!added.hasValue()) {
        return fail("document", added.error());
    }
    const std::array grants {
        requiredId<CapabilityId>("document.read"),
        requiredId<CapabilityId>("document.write")};
    auto granted = kernel.capabilities().replace(session, grants);
    if(!granted.hasValue()) {
        return fail("capability", granted.error());
    }

    auto putArguments = makeObjectSchema("schema.contract.put.arguments", true);
    auto objectResult = makeObjectSchema("schema.contract.object.result", false);
    auto getArguments = makeObjectSchema("schema.contract.get.arguments", false);
    auto getResult = makeObjectSchema("schema.contract.get.result", true);
    if(!putArguments.hasValue() || !objectResult.hasValue()
       || !getArguments.hasValue() || !getResult.hasValue()) {
        std::cerr << "schema: contract schema creation failed\n";
        return 1;
    }
    auto registeredCommand = lasercnc::test::registerCommand(kernel,
        CommandDescriptor {
            requiredId<CommandName>("kernel.contract.object.put"),
            Version {1U, 0U, 0U},
            std::move(putArguments).value(),
            std::move(objectResult).value(),
            ExecutionMode::Synchronous,
            SideEffectLevel::DocumentWrite,
            requiredId<CapabilityId>("document.write"),
            false,
            true,
            true},
        std::make_shared<PutObjectHandler>());
    if(!registeredCommand.hasValue()) {
        return fail("command registration", registeredCommand.error());
    }
    auto registeredQuery = lasercnc::test::registerQuery(kernel,
        QueryDescriptor {
            requiredId<QueryName>("kernel.contract.object.get"),
            Version {1U, 0U, 0U},
            std::move(getArguments).value(),
            std::move(getResult).value(),
            requiredId<CapabilityId>("document.read"),
            ExecutionScope::Document,
            true},
        std::make_shared<GetObjectHandler>());
    if(!registeredQuery.hasValue()) {
        return fail("query registration", registeredQuery.error());
    }
    std::size_t eventCount = 0U;
    auto subscription = kernel.events().subscribe(
        requiredId<SubscriptionId>("subscription.headless-contract"),
        EventFilter {EventKind::Domain, requiredId<EventName>("kernel.contract.object-created")},
        DeliveryMode::Immediate,
        [&](const EventEnvelope&) { ++eventCount; });
    if(!subscription.hasValue()) {
        return fail("subscription", subscription.error());
    }
    auto bootstrapped = kernel.bootstrap();
    if(!bootstrapped.hasValue()) {
        return fail("bootstrap", bootstrapped.error());
    }
    const auto catalog = kernel.execution().catalog();
    const auto commandFound = std::ranges::any_of(
        catalog.commands,
        [](const CommandDescriptor& descriptor) {
            return descriptor.name
                == requiredId<CommandName>("kernel.contract.object.put");
        });
    const auto queryFound = std::ranges::any_of(
        catalog.queries,
        [](const QueryDescriptor& descriptor) {
            return descriptor.name
                == requiredId<QueryName>("kernel.contract.object.get");
        });
    if(!commandFound || !queryFound) {
        std::cerr << "discovery: deterministic descriptors are missing\n";
        return 1;
    }

    auto parsedArguments = validator->deserialize(R"({"id":"object.cli","data":"verified"})");
    if(!parsedArguments.hasValue()) {
        return fail("argument parse", parsedArguments.error());
    }
    auto command = kernel.execution().executeCommand(CommandRequest {
        requiredId<RequestId>("request.cli.command"),
        ExecutionContext {session, project, document},
        requiredId<CommandName>("kernel.contract.object.put"),
        Version {1U, 0U, 0U},
        std::move(parsedArguments).value(),
        Revision {0U},
        requiredId<CorrelationId>("correlation.cli"),
        requiredId<TraceId>("trace.cli"),
        requiredId<IdempotencyKey>("idempotency.cli.put")});
    if(!command.hasValue()) {
        return fail("command execute", command.error());
    }
    if(eventCount != 1U || command.value().replayed
       || !command.value().postExecutionErrors.empty()) {
        std::cerr << "command execute: invalid commit or event result\n";
        return 1;
    }

    auto queryArguments = validator->deserialize(R"({"id":"object.cli"})");
    if(!queryArguments.hasValue()) {
        return fail("query parse", queryArguments.error());
    }
    auto query = kernel.execution().executeQuery(QueryRequest {
        requiredId<RequestId>("request.cli.query"),
        ExecutionContext {session, project, document},
        requiredId<QueryName>("kernel.contract.object.get"),
        Version {1U, 0U, 0U},
        std::move(queryArguments).value(),
        requiredId<CorrelationId>("correlation.cli"),
        requiredId<TraceId>("trace.cli")});
    if(!query.hasValue()) {
        return fail("query execute", query.error());
    }
    auto serialized = validator->serialize(query.value().result);
    if(!serialized.hasValue()) {
        return fail("result serialize", serialized.error());
    }
    auto flushed = logService->flush();
    if(!flushed.hasValue()) {
        return fail("log flush", flushed.error());
    }
    if(!std::filesystem::exists(logPath) || std::filesystem::file_size(logPath) == 0U
       || !containsObservabilityJsonl(logPath)) {
        std::cerr << "logging: JSONL output is missing\n";
        return 1;
    }
    auto stopped = kernel.shutdown();
    if(!stopped.hasValue()) {
        return fail("shutdown", stopped.error());
    }

    std::cout << serialized.value() << '\n';
    static_cast<void>(std::filesystem::remove(logPath, ignored));
    return serialized.value().find("verified") == std::string::npos ? 1 : 0;
}

int runTaskRoundTrip()
{
    const auto logPath = uniqueLogPath();
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove(logPath, ignored));
    SpdlogLogOptions options;
    options.enableConsole = false;
    options.jsonlFilePath = logPath;
    auto logger = SpdlogLogService::create(options);
    if(!logger) {
        return fail("logger", logger.error());
    }
    auto validator = std::make_shared<JsonconsAdapter>();
    std::shared_ptr<lasercnc::observability::ILogService> logService(
        std::move(logger).value());

    lasercnc::kernel::AppKernel kernel;
    auto configured = kernel.executionServices().configure(validator, logService);
    if(!configured) {
        return fail("configure", configured.error());
    }
    auto observability = configureObservability(kernel, logService);
    if(!observability) {
        return fail("observability", observability.error());
    }
    const auto project = requiredId<ProjectId>("project.headless-task");
    const auto document = requiredId<DocumentId>("document.headless-task");
    const auto session = requiredId<SessionId>("session.cli-task");
    auto added = kernel.addDocument(project, document);
    if(!added) {
        return fail("document", added.error());
    }
    const std::array grants {requiredId<CapabilityId>("task.submit")};
    auto granted = kernel.capabilities().replace(session, grants);
    if(!granted) {
        return fail("capability", granted.error());
    }

    auto arguments = makeObjectSchema("schema.contract.task.arguments", false);
    auto taskResult = makeObjectSchema("schema.contract.task.result", true);
    auto acceptance = makeObjectSchema("schema.contract.task.acceptance", false);
    if(!arguments || !taskResult || !acceptance) {
        std::cerr << "schema: task contract schema creation failed\n";
        return 1;
    }
    auto taskRegistered = lasercnc::test::registerTask(kernel,
        TaskDescriptor {
            requiredId<TaskName>("kernel.contract.compute"),
            Version {1U, 0U, 0U},
            arguments.value(),
            std::move(taskResult).value()},
        std::make_shared<ContractTaskHandler>());
    if(!taskRegistered) {
        return fail("task registration", taskRegistered.error());
    }
    auto commandRegistered = lasercnc::test::registerAsyncCommand(kernel,
        CommandDescriptor {
            requiredId<CommandName>("kernel.contract.compute.accept"),
            Version {1U, 0U, 0U},
            std::move(arguments).value(),
            std::move(acceptance).value(),
            ExecutionMode::Asynchronous,
            SideEffectLevel::ReadOnly,
            requiredId<CapabilityId>("task.submit"),
            false,
            true,
            true},
        std::make_shared<ContractAsyncCommandHandler>());
    if(!commandRegistered) {
        return fail("async command registration", commandRegistered.error());
    }
    auto executor = BsThreadPoolExecutor::create(BsThreadPoolExecutorOptions {2U});
    if(!executor) {
        return fail("executor", executor.error());
    }
    auto executorConfigured = kernel.configureTaskExecutor(std::move(executor).value());
    if(!executorConfigured) {
        return fail("executor configure", executorConfigured.error());
    }
    auto bootstrapped = kernel.bootstrap();
    if(!bootstrapped) {
        return fail("bootstrap", bootstrapped.error());
    }

    auto parsed = validator->deserialize(R"({"id":"input.cli.task"})");
    if(!parsed) {
        return fail("argument parse", parsed.error());
    }
    auto accepted = kernel.execution().executeCommand(CommandRequest {
        requiredId<RequestId>("request.cli.task"),
        ExecutionContext {session, project, document},
        requiredId<CommandName>("kernel.contract.compute.accept"),
        Version {1U, 0U, 0U},
        std::move(parsed).value(),
        Revision {0U},
        requiredId<CorrelationId>("correlation.cli.task"),
        requiredId<TraceId>("trace.cli.task"),
        requiredId<IdempotencyKey>("idempotency.cli.task")});
    if(!accepted) {
        return fail("async command execute", accepted.error());
    }
    if(accepted.value().commit.has_value() || !accepted.value().taskId.has_value()
       || !accepted.value().postExecutionErrors.empty()) {
        std::cerr << "async command execute: invalid acceptance\n";
        return 1;
    }
    auto completed = kernel.execution().waitTask(*accepted.value().taskId, std::chrono::seconds(2));
    if(!completed) {
        return fail("task wait", completed.error());
    }
    if(completed.value().state != TaskState::Succeeded
       || !completed.value().sourceRevisions.has_value()
       || !completed.value().result.has_value()) {
        std::cerr << "task wait: invalid terminal state\n";
        return 1;
    }
    const auto spans = kernel.traces().records();
    const auto commandSpan = std::find_if(spans.begin(), spans.end(), [](const auto& span) {
        return span.name == "command.execute";
    });
    const auto taskSpan = std::find_if(spans.begin(), spans.end(), [](const auto& span) {
        return span.name == "task.execute";
    });
    if(commandSpan == spans.end() || taskSpan == spans.end()
       || !taskSpan->parentSpanId.has_value()
       || *taskSpan->parentSpanId != commandSpan->spanId
       || taskSpan->traceId != commandSpan->traceId) {
        std::cerr << "observability: command/task trace hierarchy is invalid\n";
        return 1;
    }
    auto serialized = validator->serialize(*completed.value().result);
    if(!serialized) {
        return fail("task result serialize", serialized.error());
    }
    auto flushed = logService->flush();
    if(!flushed) {
        return fail("log flush", flushed.error());
    }
    if(!containsObservabilityJsonl(logPath)) {
        std::cerr << "observability: JSONL span or metric output is missing\n";
        return 1;
    }
    auto stopped = kernel.shutdown();
    if(!stopped) {
        return fail("shutdown", stopped.error());
    }
    std::cout << serialized.value() << '\n';
    static_cast<void>(std::filesystem::remove(logPath, ignored));
    return serialized.value().find("task-verified") == std::string::npos ? 1 : 0;
}

int seedPersistence(const std::filesystem::path& stateRoot)
{
    lasercnc::kernel::AppKernel kernel;
    const auto project = requiredId<ProjectId>("project.persistence-contract");
    const auto document = requiredId<DocumentId>("document.persistence-contract");
    auto added = kernel.addDocument(project, document);
    if(!added) {
        return fail("persistence document", added.error());
    }
    auto handler = std::make_shared<PutObjectHandler>();
    auto configured = configurePersistenceContract(kernel, stateRoot, handler);
    if(!configured) {
        return fail("persistence configure", configured.error());
    }
    auto bootstrapped = kernel.bootstrap();
    if(!bootstrapped) {
        return fail("persistence bootstrap", bootstrapped.error());
    }
    auto first = kernel.execution().executeCommand(persistenceCommand(
        "request.persistence.first",
        "object.persistence.snapshot",
        "snapshot",
        Revision {0U},
        "idempotency.persistence.first"));
    if(!first || !first.value().commit.has_value()) {
        return first ? 1 : fail("persistence first command", first.error());
    }
    auto image = kernel.documents().snapshot(document);
    if(!image) {
        return fail("persistence snapshot source", image.error());
    }
    auto captured = kernel.persistence().captureSnapshot(
        requiredId<SnapshotId>("snapshot.persistence-contract"), image.value());
    if(!captured) {
        return fail("persistence snapshot", captured.error());
    }
    auto second = kernel.execution().executeCommand(persistenceCommand(
        "request.persistence.second",
        "object.persistence.tail",
        "journal-tail",
        Revision {1U},
        "idempotency.persistence.second"));
    if(!second || !second.value().commit.has_value()) {
        return second ? 1 : fail("persistence second command", second.error());
    }
    auto latest = kernel.documents().snapshot(document);
    if(!latest) {
        return fail("persistence latest document", latest.error());
    }
    auto accepted = kernel.persistence().acceptTask(
        TaskRequest {
            requiredId<TaskId>("task.persistence.interrupted"),
            requiredId<TaskName>("kernel.persistence.interrupted"),
            Value {Value::Object {}},
            requiredId<TraceId>("trace.persistence.interrupted"),
            std::nullopt,
            project,
            document},
        latest.value().revisions());
    if(!accepted) {
        return fail("persistence interrupted task", accepted.error());
    }
    auto diagnostic = kernel.diagnostics().run(
        requiredId<DiagnosticId>("kernel.contract.persistence"));
    if(!diagnostic) {
        return fail("persistence diagnostic", diagnostic.error());
    }
    std::cout << "persistence-seeded\n" << std::flush;
    std::_Exit(EXIT_SUCCESS);
}

int recoverPersistence(const std::filesystem::path& stateRoot)
{
    lasercnc::kernel::AppKernel kernel;
    auto handler = std::make_shared<PutObjectHandler>();
    auto configured = configurePersistenceContract(kernel, stateRoot, handler);
    if(!configured) {
        return fail("recovery configure", configured.error());
    }
    std::size_t eventCount = 0U;
    auto subscription = kernel.events().subscribe(
        requiredId<SubscriptionId>("subscription.persistence-recovery"),
        EventFilter {
            EventKind::Domain,
            requiredId<EventName>("kernel.contract.object-created")},
        DeliveryMode::Immediate,
        [&](const EventEnvelope&) { ++eventCount; });
    if(!subscription) {
        return fail("recovery subscription", subscription.error());
    }
    auto bootstrapped = kernel.bootstrap();
    if(!bootstrapped) {
        return fail("recovery bootstrap", bootstrapped.error());
    }
    auto snapshotObject = queryPersistentObject(
        kernel, "request.persistence.query-snapshot", "object.persistence.snapshot");
    auto tailObject = queryPersistentObject(
        kernel, "request.persistence.query-tail", "object.persistence.tail");
    if(!snapshotObject || !tailObject) {
        return !snapshotObject
            ? fail("recovery snapshot object", snapshotObject.error())
            : fail("recovery tail object", tailObject.error());
    }
    auto replay = kernel.execution().executeCommand(persistenceCommand(
        "request.persistence.first-retry",
        "object.persistence.snapshot",
        "snapshot",
        Revision {0U},
        "idempotency.persistence.first"));
    if(!replay) {
        return fail("recovery idempotency", replay.error());
    }
    if(!replay.value().replayed || !replay.value().commit.has_value()
       || handler->calls.load() != 0U || eventCount != 0U) {
        std::cerr << "recovery idempotency: handler or event was replayed\n";
        return 1;
    }
    auto task = kernel.persistence().taskHistory(
        requiredId<TaskId>("task.persistence.interrupted"));
    if(!task || !task.value().has_value()
       || task.value()->state != TaskState::Failed
       || !task.value()->error.has_value()
       || std::string(task.value()->error->code.value())
           != "Task.InterruptedByRestart") {
        std::cerr << "recovery task: interrupted task history is invalid\n";
        return 1;
    }
    auto diagnostics = kernel.persistence().diagnosticHistory(
        requiredId<DiagnosticId>("kernel.contract.persistence"));
    if(!diagnostics || diagnostics.value().size() != 1U
       || diagnostics.value().front().status != DiagnosticStatus::Healthy) {
        std::cerr << "recovery diagnostics: durable report is missing\n";
        return 1;
    }
    auto stopped = kernel.shutdown();
    if(!stopped) {
        return fail("recovery shutdown", stopped.error());
    }
    std::cout << "persistence-recovered\n";
    return 0;
}

int seedWorkflowRecovery(const std::filesystem::path& stateRoot)
{
    lasercnc::kernel::AppKernel kernel;
    const auto project = requiredId<ProjectId>("project.persistence-contract");
    const auto document = requiredId<DocumentId>("document.persistence-contract");
    auto added = kernel.addDocument(project, document);
    if(!added) {
        return fail("workflow seed document", added.error());
    }
    auto handler = std::make_shared<PutObjectHandler>();
    auto configured = configurePersistenceContract(kernel, stateRoot, handler);
    if(!configured) {
        return fail("workflow seed configure", configured.error());
    }
    auto bootstrapped = kernel.bootstrap();
    if(!bootstrapped) {
        return fail("workflow seed bootstrap", bootstrapped.error());
    }
    auto documentSnapshot = kernel.documents().snapshot(document);
    if(!documentSnapshot) {
        return fail("workflow seed document snapshot", documentSnapshot.error());
    }
    auto captured = kernel.persistence().captureSnapshot(
        requiredId<SnapshotId>("snapshot.persistence-workflow"),
        documentSnapshot.value());
    if(!captured) {
        return fail("workflow seed snapshot", captured.error());
    }

    const auto request = persistenceWorkflowRequest();
    auto started = kernel.execution().startWorkflow(request);
    if(!started) {
        return fail("workflow seed start", started.error());
    }
    auto interrupted = started.value();
    interrupted.state = WorkflowState::Running;
    interrupted.steps.front().state = WorkflowStepState::Running;
    interrupted.steps.front().attempt = 1U;
    auto checkpointed = kernel.persistence().saveWorkflowCheckpoint(
        request, persistenceWorkflowDefinition(), interrupted, {});
    if(!checkpointed) {
        return fail("workflow seed running checkpoint", checkpointed.error());
    }
    if(handler->calls.load() != 0U) {
        std::cerr << "workflow seed: handler ran before the interrupted checkpoint\n";
        return 1;
    }
    std::cout << "workflow-recovery-seeded\n" << std::flush;
    std::_Exit(EXIT_SUCCESS);
}

int recoverWorkflow(const std::filesystem::path& stateRoot)
{
    lasercnc::kernel::AppKernel kernel;
    auto handler = std::make_shared<PutObjectHandler>();
    auto configured = configurePersistenceContract(kernel, stateRoot, handler);
    if(!configured) {
        return fail("workflow recovery configure", configured.error());
    }
    std::size_t eventCount = 0U;
    auto subscription = kernel.events().subscribe(
        requiredId<SubscriptionId>("subscription.workflow-recovery"),
        EventFilter {
            EventKind::Domain,
            requiredId<EventName>("kernel.contract.object-created")},
        DeliveryMode::Immediate,
        [&](const EventEnvelope&) { ++eventCount; });
    if(!subscription) {
        return fail("workflow recovery subscription", subscription.error());
    }
    auto bootstrapped = kernel.bootstrap();
    if(!bootstrapped) {
        return fail("workflow recovery bootstrap", bootstrapped.error());
    }
    const auto request = persistenceWorkflowRequest();
    auto restored = kernel.execution().workflow(request.workflowId);
    if(!restored) {
        return fail("workflow recovery snapshot", restored.error());
    }
    if(restored.value().state != WorkflowState::Waiting
       || restored.value().steps.front().state != WorkflowStepState::Waiting
       || restored.value().steps.front().attempt != 1U
       || handler->calls.load() != 0U) {
        std::cerr << "workflow recovery: interrupted attempt was not restored safely state="
                  << static_cast<int>(restored.value().state)
                  << " step=" << static_cast<int>(restored.value().steps.front().state)
                  << " attempt=" << restored.value().steps.front().attempt
                  << " calls=" << handler->calls.load() << '\n';
        return 1;
    }
    auto completed = kernel.execution().advanceWorkflow(request.workflowId);
    if(!completed) {
        return fail("workflow recovery advance", completed.error());
    }
    if(completed.value().state != WorkflowState::Succeeded
       || completed.value().steps.front().attempt != 1U
       || handler->calls.load() != 1U || eventCount != 1U) {
        std::cerr << "workflow recovery: attempt was not completed exactly once\n";
        return 1;
    }
    auto object = queryPersistentObject(
        kernel, "request.persistence.query-workflow", "object.persistence.workflow");
    if(!object) {
        return fail("workflow recovery object", object.error());
    }
    auto replay = kernel.execution().advanceWorkflow(request.workflowId);
    if(!replay || replay.value().state != WorkflowState::Succeeded
       || handler->calls.load() != 1U || eventCount != 1U) {
        std::cerr << "workflow recovery: terminal advance replayed a side effect\n";
        return 1;
    }
    const auto* result = object.value().getIf<Value::Object>();
    const auto* data = result == nullptr ? nullptr : result->at("data").getIf<std::string>();
    if(data == nullptr || *data != "workflow-recovered") {
        std::cerr << "workflow recovery: recovered command result is invalid\n";
        return 1;
    }
    auto stopped = kernel.shutdown();
    if(!stopped) {
        return fail("workflow recovery shutdown", stopped.error());
    }
    std::cout << "workflow-recovery-completed\n";
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if((argc != 3 && argc != 5) || std::string(argv[1]) != "--mode") {
        std::cerr << "usage: lasercnc_kernel_headless_contract --mode "
                     "roundtrip|task-roundtrip|persistence-seed|persistence-recover|"
                     "workflow-recovery-seed|workflow-recovery-recover "
                     "[--state-root path]\n";
        return 2;
    }
    try {
        const auto mode = std::string(argv[2]);
        if(mode == "roundtrip") {
            return runRoundTrip();
        }
        if(mode == "task-roundtrip") {
            return runTaskRoundTrip();
        }
        if((mode == "persistence-seed" || mode == "persistence-recover")
           && argc == 5 && std::string(argv[3]) == "--state-root") {
            return mode == "persistence-seed"
                ? seedPersistence(std::filesystem::path {argv[4]})
                : recoverPersistence(std::filesystem::path {argv[4]});
        }
        if((mode == "workflow-recovery-seed" || mode == "workflow-recovery-recover")
           && argc == 5 && std::string(argv[3]) == "--state-root") {
            return mode == "workflow-recovery-seed"
                ? seedWorkflowRecovery(std::filesystem::path {argv[4]})
                : recoverWorkflow(std::filesystem::path {argv[4]});
        }
        std::cerr << "unknown mode\n";
        return 2;
    } catch(const std::exception& exception) {
        std::cerr << "unexpected: " << exception.what() << '\n';
        return 3;
    } catch(...) {
        std::cerr << "unexpected: unknown failure\n";
        return 3;
    }
}
