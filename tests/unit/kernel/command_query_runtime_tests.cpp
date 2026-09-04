#include <lasercnc/kernel/app_kernel.hpp>
#include <lasercnc/observability/log_service.hpp>
#include <lasercnc/observability/metrics_service.hpp>
#include <lasercnc/observability/trace_service.hpp>
#include <lasercnc/runtime/command_runtime.hpp>
#include <lasercnc/runtime/query_runtime.hpp>

#include <catch2/catch_test_macros.hpp>
#include "kernel_test_module.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace lasercnc::foundation;
using namespace lasercnc::kernel;
using namespace lasercnc::messaging;
using namespace lasercnc::observability;
using namespace lasercnc::runtime;
using namespace lasercnc::state;

namespace {

template <typename Id>
Id validId(const char* value)
{
    auto created = Id::create(value);
    if(!created.hasValue()) {
        throw std::logic_error("Invalid test ID");
    }
    return std::move(created).value();
}

Schema schema(const char* id, SchemaKind kind)
{
    auto created = Schema::create(validId<SchemaId>(id), Version {1U, 0U, 0U}, kind);
    if(!created.hasValue()) {
        throw std::logic_error("Invalid test schema");
    }
    return std::move(created).value();
}

class RootKindValidator final : public ISchemaValidator {
public:
    Result<void> validate(const Schema& expected, const Value& value) const override
    {
        bool matches = expected.rootKind() == SchemaKind::Any;
        switch(expected.rootKind()) {
        case SchemaKind::Any: matches = true; break;
        case SchemaKind::Null: matches = value.kind() == Value::Kind::Null; break;
        case SchemaKind::Boolean: matches = value.kind() == Value::Kind::Boolean; break;
        case SchemaKind::Integer: matches = value.kind() == Value::Kind::Integer; break;
        case SchemaKind::Number: matches = value.kind() == Value::Kind::Number; break;
        case SchemaKind::String: matches = value.kind() == Value::Kind::String; break;
        case SchemaKind::Array: matches = value.kind() == Value::Kind::Array; break;
        case SchemaKind::Object: matches = value.kind() == Value::Kind::Object; break;
        }
        if(matches) {
            return Result<void>::success();
        }
        return Result<void>::failure(makeError(
            "Runtime.SchemaInvalid", ErrorCategory::Validation, "Root kind mismatch"));
    }
};

class RecordingLogService final : public ILogService {
public:
    Result<void> write(const LogRecord& record) override
    {
        std::lock_guard lock(mutex);
        if(failWrites) {
            return Result<void>::failure(makeError(
                "Test.LogFailed", ErrorCategory::Infrastructure, "Injected log failure"));
        }
        records.push_back(record);
        return Result<void>::success();
    }

    Result<void> flush() override { return Result<void>::success(); }

    std::mutex mutex;
    std::vector<LogRecord> records;
    bool failWrites{false};
};

class FailingTraceExporter final : public ITraceExporter {
public:
    Result<void> exportSpan(const TraceSpanRecord&) override
    {
        return Result<void>::failure(makeError(
            "Test.TraceExportFailed", ErrorCategory::Infrastructure, "expected"));
    }
};

class ShutdownTraceExporter final : public ITraceExporter {
public:
    explicit ShutdownTraceExporter(AppKernel& kernel) : kernel_(kernel) {}
    Result<void> exportSpan(const TraceSpanRecord&) override
    {
        if(!stopped.has_value()) { stopped.emplace(kernel_.shutdown()); }
        return Result<void>::success();
    }
    std::optional<Result<void>> stopped;
private:
    AppKernel& kernel_;
};

class AdmissionStopModule final : public IModule {
public:
    explicit AdmissionStopModule(std::function<Result<void>(AppKernel&)> callback) : callback_(std::move(callback)) {}
    const ModuleDescriptor& descriptor() const noexcept override { return descriptor_; }
    Result<void> stop(AppKernel& kernel) override { return callback_(kernel); }
private:
    ModuleDescriptor descriptor_{validId<ModuleId>("kernel.admission.stop"), "Admission stop probe", {1U, 0U, 0U}};
    std::function<Result<void>(AppKernel&)> callback_;
};

class FailingMetricsExporter final : public IMetricsExporter {
public:
    Result<void> exportObservation(const MetricObservation&) override
    {
        return Result<void>::failure(makeError(
            "Test.MetricsExportFailed", ErrorCategory::Infrastructure, "expected"));
    }
};

class CreateObjectHandler final : public ICommandHandler {
public:
    Result<Value> execute(const CommandRequest& request, ApplicationTransaction& transaction) override
    {
        ++calls;
        const auto* arguments = request.arguments.getIf<Value::Object>();
        const auto* id = arguments == nullptr ? nullptr : arguments->at("id").getIf<std::string>();
        const auto* data = arguments == nullptr ? nullptr : arguments->at("data").getIf<std::string>();
        if(id == nullptr || data == nullptr) {
            return Result<Value>::failure(makeError(
                "Test.InvalidArguments", ErrorCategory::Validation, "Invalid create arguments"));
        }
        auto objectId = ObjectId::create(*id);
        if(!objectId.hasValue()) {
            return Result<Value>::failure(std::move(objectId).error());
        }
        const auto stableObjectId = objectId.value();
        auto created = transaction.createObject(ObjectRecord {
            std::move(objectId).value(), validId<ObjectTypeId>("kernel.runtime.test"), Value {*data}});
        if(!created.hasValue()) {
            return Result<Value>::failure(std::move(created).error());
        }
        auto event = transaction.collectEvent(PendingDomainEvent {
            validId<EventName>("kernel.object-created"),
            Version {1U, 0U, 0U},
            stableObjectId,
            Value {Value::Object {{"id", Value {*id}}}}});
        if(!event.hasValue()) {
            return Result<Value>::failure(std::move(event).error());
        }
        return Result<Value>::success(Value {Value::Object {{"id", Value {*id}}}});
    }

    std::atomic_size_t calls{0U};
};

class SlowCreateHandler final : public ICommandHandler {
public:
    SlowCreateHandler(std::promise<void>& enteredPromise, std::shared_future<void> releaseFuture)
        : entered(enteredPromise), release(std::move(releaseFuture))
    {
    }

    Result<Value> execute(const CommandRequest&, ApplicationTransaction& transaction) override
    {
        ++calls;
        entered.set_value();
        release.wait();
        auto created = transaction.createObject(ObjectRecord {
            validId<ObjectId>("object.concurrent-idempotency"),
            validId<ObjectTypeId>("kernel.runtime.test"),
            Value {"created"}});
        if(!created.hasValue()) {
            return Result<Value>::failure(std::move(created).error());
        }
        return Result<Value>::success(Value {Value::Object {
            {"id", Value {"object.concurrent-idempotency"}}}});
    }

    std::atomic_size_t calls{0U};
    std::promise<void>& entered;
    std::shared_future<void> release;
};

class ObjectQueryHandler final : public IQueryHandler {
public:
    Result<Value> execute(const QueryRequest& request, const QueryContext& context) override
    {
        ++calls;
        if(!context.document.has_value()) {
            return Result<Value>::failure(makeError(
                "Test.DocumentMissing", ErrorCategory::Internal, "Document snapshot missing"));
        }
        const auto* arguments = request.arguments.getIf<Value::Object>();
        const auto* id = arguments == nullptr ? nullptr : arguments->at("id").getIf<std::string>();
        if(id == nullptr) {
            return Result<Value>::failure(makeError(
                "Test.InvalidArguments", ErrorCategory::Validation, "Invalid query arguments"));
        }
        auto objectId = ObjectId::create(*id);
        if(!objectId.hasValue()) {
            return Result<Value>::failure(std::move(objectId).error());
        }
        const auto* object = context.document->objects().find(objectId.value());
        if(object == nullptr) {
            return Result<Value>::failure(makeError(
                "Test.ObjectMissing", ErrorCategory::NotFound, "Object missing"));
        }
        return Result<Value>::success(Value {Value::Object {
            {"id", Value {*id}},
            {"data", object->data}}});
    }

    std::atomic_size_t calls{0U};
};

class BlockingQueryHandler final : public IQueryHandler {
public:
    BlockingQueryHandler(std::promise<void>& enteredPromise, std::shared_future<void> releaseFuture)
        : entered(enteredPromise), release(std::move(releaseFuture))
    {
    }

    Result<Value> execute(const QueryRequest&, const QueryContext&) override
    {
        entered.set_value();
        release.wait();
        return Result<Value>::success(Value {Value::Object {}});
    }

    std::promise<void>& entered;
    std::shared_future<void> release;
};

class ScopeQueryHandler final : public IQueryHandler {
public:
    Result<Value> execute(const QueryRequest&, const QueryContext& context) override
    {
        ++calls;
        return Result<Value>::success(Value {Value::Object {
            {"hasDocument", Value {context.document.has_value()}}}});
    }

    std::atomic_size_t calls{0U};
};

class ScopeCommandHandler final : public IReadOnlyCommandHandler {
public:
    Result<Value> execute(
        const CommandRequest&,
        const ReadOnlyCommandContext& context) override
    {
        ++calls;
        return Result<Value>::success(Value {Value::Object {
            {"hasDocument", Value {context.document.has_value()}}}});
    }

    std::atomic_size_t calls{0U};
};

class ProjectActivityProbe final : public IReadOnlyCommandHandler, public IQueryHandler {
public:
    Result<Value> execute(const CommandRequest&, const ReadOnlyCommandContext&) override { return run(); }
    Result<Value> execute(const QueryRequest&, const QueryContext&) override { return run(); }
    std::function<void()> probe;
    unsigned int calls{0U};
private:
    Result<Value> run()
    {
        ++calls;
        if(probe) { probe(); }
        return Result<Value>::success(Value{Value::Object{}});
    }
};

class ProjectTraceProbe final : public ITraceExporter {
public:
    Result<void> exportSpan(const TraceSpanRecord&) override
    {
        if(probe) { auto once = std::exchange(probe, {}); once(); }
        return Result<void>::success();
    }
    std::function<void()> probe;
};

class FailingHandler final : public ICommandHandler {
public:
    explicit FailingHandler(bool shouldThrow) : throws(shouldThrow) {}

    Result<Value> execute(const CommandRequest&, ApplicationTransaction& transaction) override
    {
        auto created = transaction.createObject(ObjectRecord {
            validId<ObjectId>("object.must-rollback"),
            validId<ObjectTypeId>("kernel.runtime.test"),
            Value {"temporary"}});
        if(!created.hasValue()) {
            return Result<Value>::failure(std::move(created).error());
        }
        if(throws) {
            throw std::runtime_error("injected handler exception");
        }
        return Result<Value>::failure(makeError(
            "Test.HandlerRejected", ErrorCategory::Validation, "Injected handler failure"));
    }

    bool throws;
};

CommandDescriptor commandDescriptor(
    const char* name,
    const char* resultSchema = "schema.command.result.object")
{
    return CommandDescriptor {
        validId<CommandName>(name),
        Version {1U, 0U, 0U},
        schema("schema.command.arguments.object", SchemaKind::Object),
        schema(resultSchema, SchemaKind::Object),
        ExecutionMode::Synchronous,
        SideEffectLevel::DocumentWrite,
        validId<CapabilityId>("document.write"),
        false,
        true,
        true};
}

QueryDescriptor queryDescriptor(
    const char* name,
    ExecutionScope scope = ExecutionScope::Document)
{
    return QueryDescriptor {
        validId<QueryName>(name),
        Version {1U, 0U, 0U},
        schema("schema.query.arguments.object", SchemaKind::Object),
        schema("schema.query.result.object", SchemaKind::Object),
        validId<CapabilityId>("document.read"),
        scope,
        true};
}

CommandRequest commandRequest(
    const char* requestId,
    const ProjectId& project,
    const DocumentId& document,
    const SessionId& session,
    const char* command,
    const char* objectId,
    std::optional<IdempotencyKey> idempotencyKey = std::nullopt,
    std::optional<Revision> expectedRevision = std::nullopt)
{
    return CommandRequest {
        validId<RequestId>(requestId),
        ExecutionContext {session, project, document},
        validId<CommandName>(command),
        Version {1U, 0U, 0U},
        Value {Value::Object {
            {"data", Value {"payload"}},
            {"id", Value {objectId}},
        }},
        expectedRevision,
        validId<CorrelationId>("correlation.runtime"),
        validId<TraceId>("trace.runtime"),
        std::move(idempotencyKey)};
}

QueryRequest queryRequest(
    const ProjectId& project,
    const DocumentId& document,
    const SessionId& session,
    const char* objectId)
{
    return QueryRequest {
        validId<RequestId>("request.query"),
        ExecutionContext {session, project, document},
        validId<QueryName>("kernel.object.get"),
        Version {1U, 0U, 0U},
        Value {Value::Object {{"id", Value {objectId}}}},
        validId<CorrelationId>("correlation.query"),
        validId<TraceId>("trace.query")};
}

struct RuntimeFixture final {
    RuntimeFixture()
        : validator(std::make_shared<RootKindValidator>()),
          log(std::make_shared<RecordingLogService>()),
          project(validId<ProjectId>("project.runtime")),
          document(validId<DocumentId>("document.runtime")),
          session(validId<SessionId>("session.runtime"))
    {
        REQUIRE(lasercnc::test::registerObjectType(kernel,
            lasercnc::test::valueObjectType("kernel.runtime.test")).hasValue());
        REQUIRE(kernel.addDocument(project, document).hasValue());
        REQUIRE(kernel.executionServices().configure(validator, log).hasValue());
        const std::array grants {
            validId<CapabilityId>("document.read"),
            validId<CapabilityId>("document.write"),
            validId<CapabilityId>("kernel.history.edit")};
        REQUIRE(kernel.capabilities().replace(session, grants).hasValue());
    }

    void registerStandardHandlers()
    {
        create = std::make_shared<CreateObjectHandler>();
        query = std::make_shared<ObjectQueryHandler>();
        REQUIRE(lasercnc::test::registerCommand(kernel,
            commandDescriptor("kernel.object.create"), create).hasValue());
        REQUIRE(lasercnc::test::registerQuery(kernel,
            queryDescriptor("kernel.object.get"), query).hasValue());
    }

    std::shared_ptr<RootKindValidator> validator;
    std::shared_ptr<RecordingLogService> log;
    lasercnc::kernel::AppKernel kernel;
    ProjectId project;
    DocumentId document;
    SessionId session;
    std::shared_ptr<CreateObjectHandler> create;
    std::shared_ptr<ObjectQueryHandler> query;
};

CommandDescriptor lifecycleDescriptor(const char* name, LifecycleOperation operation)
{
    auto descriptor = commandDescriptor(name);
    descriptor.sideEffect = SideEffectLevel::LifecycleControl;
    descriptor.idempotent = false;
    descriptor.deterministic = false;
    descriptor.lifecycleOperation = operation;
    descriptor.scope = operation == LifecycleOperation::ProjectCreate || operation == LifecycleOperation::ProjectOpen
        || operation == LifecycleOperation::ProjectClose ? ExecutionScope::Project : ExecutionScope::Document;
    descriptor.capability = validId<CapabilityId>("kernel.lifecycle.control");
    return descriptor;
}

const std::array lifecycleOperations{
    std::pair{"kernel.project.create", LifecycleOperation::ProjectCreate},
    std::pair{"kernel.project.open", LifecycleOperation::ProjectOpen},
    std::pair{"kernel.project.close", LifecycleOperation::ProjectClose},
    std::pair{"kernel.document.create", LifecycleOperation::DocumentCreate},
    std::pair{"kernel.document.open", LifecycleOperation::DocumentOpen},
    std::pair{"kernel.document.close", LifecycleOperation::DocumentClose},
    std::pair{"kernel.document.remove", LifecycleOperation::DocumentRemove}};

void installLifecycleCommands(RuntimeFixture& fixture)
{
    REQUIRE(lasercnc::test::installKernelTestModule(fixture.kernel, [](auto& builder) {
        for(const auto& [name, operation] : lifecycleOperations) {
            builder.lifecycleCommand(lifecycleDescriptor(name, operation));
        }
    }));
    const std::array grants{validId<CapabilityId>("document.read"), validId<CapabilityId>("document.write"),
        validId<CapabilityId>("kernel.lifecycle.control")};
    REQUIRE(fixture.kernel.capabilities().replace(fixture.session, grants));
}

CommandRequest lifecycleRequest(RuntimeFixture& fixture, const char* name, bool projectOnly = false)
{
    auto request = commandRequest("request.lifecycle", fixture.project, fixture.document, fixture.session, name, "unused");
    request.arguments = Value{Value::Object{}};
    if(projectOnly) { request.context.documentId.reset(); }
    return request;
}

template <typename Registrar>
concept CanInjectLifecycleHandler = requires(Registrar& registrar, CommandDescriptor descriptor,
    std::shared_ptr<IReadOnlyCommandHandler> handler) { registrar.registerLifecycleCommand(descriptor, handler); };
static_assert(!CanInjectLifecycleHandler<ModuleRegistrar>);
static_assert(!CanInjectLifecycleHandler<CommandRegistry>);

} // namespace

TEST_CASE("AppKernel runs one headless command and query chain", "[runtime][command][query]")
{
    RuntimeFixture fixture;
    REQUIRE(fixture.kernel.traces()
                .addExporter(std::make_shared<FailingTraceExporter>())
                .hasValue());
    REQUIRE(fixture.kernel.metrics()
                .addExporter(std::make_shared<FailingMetricsExporter>())
                .hasValue());
    fixture.registerStandardHandlers();
    std::vector<EventEnvelope> events;
    auto subscription = fixture.kernel.events().subscribe(
        validId<SubscriptionId>("subscription.runtime"),
        EventFilter {EventKind::Domain, validId<EventName>("kernel.object-created")},
        DeliveryMode::Immediate,
        [&](const EventEnvelope& event) { events.push_back(event); });
    REQUIRE(subscription.hasValue());

    auto beforeReady = fixture.kernel.execution().executeCommand(commandRequest(
        "request.before-ready", fixture.project, fixture.document, fixture.session,
        "kernel.object.create", "object.before-ready"));
    REQUIRE_FALSE(beforeReady.hasValue());
    CHECK(std::string(beforeReady.error().code.value()) == "Command.RuntimeNotReady");

    REQUIRE(fixture.kernel.bootstrap().hasValue());
    CHECK(fixture.kernel.commandRegistry().frozen());
    CHECK(fixture.kernel.queryRegistry().frozen());
    CHECK(fixture.kernel.executionServices().frozen());
    CHECK(fixture.kernel.traces().frozen());
    CHECK(fixture.kernel.metrics().frozen());
    CHECK(fixture.kernel.diagnostics().frozen());
    CHECK_FALSE(fixture.kernel.executionServices().configure(
        fixture.validator, fixture.log).hasValue());
    CHECK_FALSE(lasercnc::test::registerCommand(fixture.kernel,
        commandDescriptor("kernel.late"), fixture.create).hasValue());

    auto createRequest = commandRequest(
        "request.create", fixture.project, fixture.document, fixture.session,
        "kernel.object.create", "object.runtime");
    createRequest.parentSpanId = validId<SpanId>("span.workflow.command-parent");
    auto command = fixture.kernel.execution().executeCommand(createRequest);
    REQUIRE(command.hasValue());
    REQUIRE(command.value().commit.has_value());
    CHECK(command.value().commit->revisionsAfter.at(RevisionScope::Project) == Revision {1U});
    CHECK(command.value().postExecutionErrors.empty());
    REQUIRE(events.size() == 1U);
    REQUIRE(events.front().correlationId().has_value());
    CHECK(*events.front().correlationId() == validId<CorrelationId>("correlation.runtime"));

    auto getRequest = queryRequest(
        fixture.project, fixture.document, fixture.session, "object.runtime");
    getRequest.traceId = validId<TraceId>("trace.runtime");
    getRequest.parentSpanId = validId<SpanId>("span.workflow.query-parent");
    auto query = fixture.kernel.execution().executeQuery(getRequest);
    REQUIRE(query.hasValue());
    REQUIRE(query.value().revisions.has_value());
    CHECK(query.value().revisions->at(RevisionScope::Document) == Revision {1U});
    const auto* result = query.value().result.getIf<Value::Object>();
    REQUIRE(result != nullptr);
    CHECK(*result->at("data").getIf<std::string>() == "payload");
    CHECK(fixture.create->calls == 1U);
    CHECK(fixture.query->calls == 1U);
    CHECK(fixture.log->records.size() == 2U);

    const auto spans = fixture.kernel.traces().records();
    REQUIRE(spans.size() == 3U);
    const auto successfulCommand = std::find_if(
        spans.begin(), spans.end(), [](const auto& span) {
            return span.name == "command.execute" && span.status == TraceStatus::Succeeded;
        });
    const auto successfulQuery = std::find_if(
        spans.begin(), spans.end(), [](const auto& span) {
            return span.name == "query.execute" && span.status == TraceStatus::Succeeded;
        });
    REQUIRE(successfulCommand != spans.end());
    REQUIRE(successfulQuery != spans.end());
    CHECK(successfulCommand->traceId == validId<TraceId>("trace.runtime"));
    REQUIRE(successfulCommand->parentSpanId.has_value());
    CHECK(*successfulCommand->parentSpanId
          == validId<SpanId>("span.workflow.command-parent"));
    CHECK(successfulQuery->traceId == successfulCommand->traceId);
    REQUIRE(successfulQuery->parentSpanId.has_value());
    CHECK(*successfulQuery->parentSpanId
          == validId<SpanId>("span.workflow.query-parent"));
    CHECK(fixture.kernel.traces().exporterFailures().size() == 3U);
    CHECK(fixture.kernel.metrics().exporterFailures().size() == 6U);

    const auto metrics = fixture.kernel.metrics().snapshot();
    CHECK(std::count_if(metrics.begin(), metrics.end(), [](const auto& metric) {
              return metric.name == validId<MetricName>("kernel.command.completed")
                  || metric.name == validId<MetricName>("kernel.command.duration_ms")
                  || metric.name == validId<MetricName>("kernel.query.completed")
                  || metric.name == validId<MetricName>("kernel.query.duration_ms");
          })
          == 6);

    REQUIRE(fixture.kernel.shutdown().hasValue());
    CHECK_FALSE(fixture.kernel.execution().executeQuery(queryRequest(
        fixture.project, fixture.document, fixture.session, "object.runtime")).hasValue());
}

TEST_CASE("Lifecycle command registration accepts only fixed governed operations", "[runtime][command][lifecycle-control]")
{
    for(const auto& [name, operation] : lifecycleOperations) {
        CommandRegistry registry;
        auto descriptor = lifecycleDescriptor(name, operation);
        REQUIRE(registry.registerLifecycleCommand(descriptor));
        auto entry = registry.descriptor({descriptor.name, descriptor.version}, VersionResolution::Exact);
        REQUIRE(entry);
        CHECK(entry.value().lifecycleOperation == operation);
        CHECK_FALSE(registry.registerLifecycleCommand(descriptor));
    }
    const std::vector<std::function<void(CommandDescriptor&)>> corruptions{
        [](auto& d) { d.lifecycleOperation.reset(); },
        [](auto& d) { d.lifecycleOperation = static_cast<LifecycleOperation>(255); },
        [](auto& d) { d.scope = ExecutionScope::Global; },
        [](auto& d) { d.scope = ExecutionScope::Session; },
        [](auto& d) { d.scope = ExecutionScope::Project; },
        [](auto& d) { d.executionMode = ExecutionMode::Asynchronous; },
        [](auto& d) { d.sideEffect = SideEffectLevel::ReadOnly; },
        [](auto& d) { d.sideEffect = SideEffectLevel::DocumentWrite; },
        [](auto& d) { d.undoable = true; },
        [](auto& d) { d.idempotent = true; },
        [](auto& d) { d.status = static_cast<ContractStatus>(255); },
        [](auto& d) { d.replayPolicy = ReplayPolicy::Safe; },
        [](auto& d) { d.effectGuards.push_back(validId<EffectGuardId>("guard.lifecycle")); },
        [](auto& d) { d.resources.push_back({ResourceKind::CPU, validId<ResourceId>("resource.lifecycle")}); },
        [](auto& d) { d.arguments = schema("schema.lifecycle.any", SchemaKind::Any); },
        [](auto& d) { d.result = schema("schema.lifecycle.any", SchemaKind::Any); }};
    for(std::size_t index = 0; index < corruptions.size(); ++index) {
        CAPTURE(index);
        CommandRegistry registry;
        auto descriptor = lifecycleDescriptor("kernel.document.close", LifecycleOperation::DocumentClose);
        corruptions[index](descriptor);
        auto rejected = registry.registerLifecycleCommand(descriptor);
        REQUIRE_FALSE(rejected);
        CHECK(std::string(rejected.error().code.value()) == "Command.InvalidLifecycleDescriptor");
        CHECK_FALSE(registry.descriptor({descriptor.name, descriptor.version}, VersionResolution::Exact));
    }
    CommandRegistry registry;
    auto descriptor = lifecycleDescriptor("kernel.document.close", LifecycleOperation::DocumentClose);
    CHECK_FALSE(registry.registerHandler(descriptor, std::make_shared<CreateObjectHandler>()));
    CHECK_FALSE(registry.registerReadOnlyHandler(descriptor, std::make_shared<ScopeCommandHandler>()));
    CHECK_FALSE(registry.registerAsyncHandler(descriptor, nullptr));
    CHECK_FALSE(registry.registerExternalEffectHandler(descriptor, nullptr));
    descriptor.lifecycleOperation.reset();
    CHECK_FALSE(registry.registerReadOnlyHandler(descriptor, std::make_shared<ScopeCommandHandler>()));
}

TEST_CASE("Lifecycle commands create open close and remove without self blocking or business commits", "[runtime][command][lifecycle-control]")
{
    RuntimeFixture fixture;
    installLifecycleCommands(fixture);
    REQUIRE(fixture.kernel.bootstrap());
    fixture.project = validId<ProjectId>("project.lifecycle.new");
    fixture.document = validId<DocumentId>("document.lifecycle.new");
    const auto run = [&](const char* name, bool projectOnly, const char* state) {
        auto result = fixture.kernel.execution().executeCommand(lifecycleRequest(fixture, name, projectOnly));
        INFO(name);
        if(!result) { INFO(std::string(result.error().code.value())); }
        REQUIRE(result);
        CHECK_FALSE(result.value().commit);
        CHECK_FALSE(result.value().taskId);
        CHECK_FALSE(result.value().replayed);
        CHECK(result.value().postExecutionErrors.empty());
        const auto* object = result.value().result.getIf<Value::Object>();
        REQUIRE(object);
        CHECK(*object->at("state").getIf<std::string>() == state);
        CHECK(*object->at("projectId").getIf<std::string>() == std::string(fixture.project.value()));
        if(!projectOnly) { CHECK(*object->at("documentId").getIf<std::string>() == std::string(fixture.document.value())); }
    };
    run("kernel.project.create", true, "open");
    run("kernel.document.create", false, "open");
    run("kernel.document.close", false, "detached");
    auto opened = fixture.kernel.execution().executeCommand(lifecycleRequest(fixture, "kernel.document.open"));
    REQUIRE_FALSE(opened);
    CHECK(std::string(opened.error().code.value()) == "Document.OpenRequiresPersistence");
    run("kernel.project.close", true, "closed");
    CHECK_FALSE(fixture.kernel.documents().contains(fixture.document));
    run("kernel.project.open", true, "open");
    run("kernel.document.remove", false, "removed");
    CHECK_FALSE(fixture.kernel.execution().executeCommand(lifecycleRequest(fixture, "kernel.document.open")));
    REQUIRE(fixture.kernel.shutdown());
}

TEST_CASE("Lifecycle commands reject untrusted targets parameters and admission metadata before mutation", "[runtime][command][lifecycle-control]")
{
    RuntimeFixture fixture;
    installLifecycleCommands(fixture);
    const auto foreign = validId<ProjectId>("project.lifecycle.foreign");
    REQUIRE(fixture.kernel.addProject(foreign));
    REQUIRE(fixture.kernel.bootstrap());
    const auto base = lifecycleRequest(fixture, "kernel.document.close");
    const std::vector<std::pair<std::function<void(CommandRequest&)>, const char*>> corruptions{
        {[](auto& r) { r.context.sessionId = validId<SessionId>("session.denied"); }, "Capability.Denied"},
        {[](auto& r) { r.context.projectId.reset(); }, "Command.ScopeMismatch"},
        {[](auto& r) { r.context.documentId.reset(); }, "Command.ScopeMismatch"},
        {[&](auto& r) { r.context.projectId = foreign; }, "Command.ProjectMismatch"},
        {[](auto& r) { r.expectedRevision = Revision{0U}; }, "Command.LifecycleRevisionUnsupported"},
        {[](auto& r) { r.idempotencyKey = validId<IdempotencyKey>("key.lifecycle"); }, "Command.IdempotencyUnsupported"},
        {[](auto& r) { r.arguments = Value{Value::Object{{"skip", Value{true}}}}; }, "Command.LifecycleArgumentsUnsupported"},
        {[](auto& r) { r.arguments = Value{Value::Object{{"documentId", Value{"document.other"}}}}; }, "Command.LifecycleArgumentsUnsupported"},
        {[](auto& r) { r.arguments = Value{true}; }, "Runtime.SchemaInvalid"}};
    for(std::size_t index = 0; index < corruptions.size(); ++index) {
        CAPTURE(index);
        auto request = base;
        corruptions[index].first(request);
        auto result = fixture.kernel.execution().executeCommand(request);
        REQUIRE_FALSE(result);
        CHECK(std::string(result.error().code.value()) == corruptions[index].second);
        CHECK(fixture.kernel.documentRuntime().lifecycle(fixture.document).value().state == DocumentLifecycleState::Open);
    }
    auto versioned = base;
    versioned.version = {2U, 0U, 0U};
    CHECK_FALSE(fixture.kernel.execution().executeCommand(versioned));
    CHECK(fixture.kernel.documentRuntime().lifecycle(fixture.document).value().state == DocumentLifecycleState::Open);
    REQUIRE(fixture.kernel.execution().executeCommand(base));
    // A repeated request is evaluated against real state, never a cached close reply.
    // 中文翻译：重复请求必须检查真实状态，不能重放缓存的关闭成功响应。
    CHECK_FALSE(fixture.kernel.execution().executeCommand(base));
    REQUIRE(fixture.kernel.shutdown());
}

TEST_CASE("Lifecycle close retains other command activities without an admission escape", "[runtime][command][lifecycle-control]")
{
    for(const auto scope : {ExecutionScope::Project, ExecutionScope::Document}) {
        RuntimeFixture fixture;
        installLifecycleCommands(fixture);
        auto probe = std::make_shared<ProjectActivityProbe>();
        auto descriptor = commandDescriptor("kernel.lifecycle.busy");
        descriptor.scope = scope;
        descriptor.sideEffect = SideEffectLevel::ReadOnly;
        descriptor.idempotent = false;
        REQUIRE(lasercnc::test::registerReadOnlyCommand(fixture.kernel, descriptor, probe));
        REQUIRE(fixture.kernel.bootstrap());
        probe->probe = [&] {
            auto closed = fixture.kernel.execution().executeCommand(lifecycleRequest(fixture,
                scope == ExecutionScope::Project ? "kernel.project.close" : "kernel.document.close",
                scope == ExecutionScope::Project));
            REQUIRE_FALSE(closed);
            CHECK(fixture.kernel.projectRuntime().lifecycle(fixture.project).value().state == ProjectLifecycleState::Open);
            CHECK(fixture.kernel.documentRuntime().lifecycle(fixture.document).value().state == DocumentLifecycleState::Open);
        };
        auto request = lifecycleRequest(fixture, "kernel.lifecycle.busy", scope == ExecutionScope::Project);
        REQUIRE(fixture.kernel.execution().executeCommand(request));
        CHECK(probe->calls == 1U);
        REQUIRE(fixture.kernel.execution().executeCommand(lifecycleRequest(fixture,
            scope == ExecutionScope::Project ? "kernel.project.close" : "kernel.document.close",
            scope == ExecutionScope::Project)));
        REQUIRE(fixture.kernel.shutdown());
    }
}

TEST_CASE("Lifecycle completion retains kernel admission and reports observer failures as post execution errors", "[runtime][command][lifecycle-control]")
{
    RuntimeFixture fixture;
    installLifecycleCommands(fixture);
    auto exporter = std::make_shared<ShutdownTraceExporter>(fixture.kernel);
    REQUIRE(fixture.kernel.traces().addExporter(exporter));
    REQUIRE(fixture.kernel.bootstrap());
    fixture.log->failWrites = true;
    auto closed = fixture.kernel.execution().executeCommand(lifecycleRequest(fixture, "kernel.document.close"));
    REQUIRE(closed);
    REQUIRE(closed.value().postExecutionErrors.size() == 1U);
    CHECK(std::string(closed.value().postExecutionErrors.front().code.value()) == "Test.LogFailed");
    CHECK(fixture.kernel.documentRuntime().lifecycle(fixture.document).value().state == DocumentLifecycleState::Detached);
    REQUIRE(exporter->stopped.has_value());
    REQUIRE_FALSE(*exporter->stopped);
    CHECK(std::string(exporter->stopped->error().code.value()) == "Kernel.ActiveExecutions");
    CHECK(fixture.kernel.state() == AppKernelState::Ready);
    fixture.log->failWrites = false;
    REQUIRE(fixture.kernel.shutdown());
}

TEST_CASE("Lifecycle contributions obey declarations rollback freeze and versioned discovery", "[kernel][modules][lifecycle-control]")
{
    SECTION("ignored undeclared or invalid contributions poison and roll back their module") {
        for(bool undeclared : {false, true}) {
            RuntimeFixture fixture;
            auto good = lifecycleDescriptor("kernel.document.close", LifecycleOperation::DocumentClose);
            auto bad = lifecycleDescriptor("kernel.document.open", LifecycleOperation::DocumentOpen);
            ModuleDescriptor module{validId<ModuleId>("module.lifecycle.invalid"), "Lifecycle test", {1U, 0U, 0U}};
            module.commands.push_back({good.name, good.version});
            if(!undeclared) { module.commands.push_back({bad.name, bad.version}); bad.scope = ExecutionScope::Global; }
            std::vector<lasercnc::test::KernelTestModule::Registration> registrations;
            registrations.push_back([good, bad](ModuleRegistrar& registrar) {
                auto first = registrar.registerLifecycleCommand(good);
                if(!first) { return first; }
                static_cast<void>(registrar.registerLifecycleCommand(bad));
                return Result<void>::success();
            });
            REQUIRE(fixture.kernel.addModule(std::make_unique<lasercnc::test::KernelTestModule>(module, registrations)));
            CHECK_FALSE(fixture.kernel.bootstrap());
            CHECK_FALSE(fixture.kernel.commandRegistry().descriptor({good.name, good.version}));
            CHECK_FALSE(fixture.kernel.commandRegistry().descriptor({bad.name, bad.version}));
        }
    }
    SECTION("catalog exposes fixed metadata and compatible resolution uses the registered operation") {
        RuntimeFixture fixture;
        auto descriptor = lifecycleDescriptor("kernel.document.close", LifecycleOperation::DocumentClose);
        descriptor.version = {1U, 1U, 0U};
        descriptor.status = ContractStatus::Deprecated;
        REQUIRE(lasercnc::test::installKernelTestModule(fixture.kernel, [&](auto& builder) {
            builder.lifecycleCommand(descriptor);
        }));
        const std::array grants{descriptor.capability};
        REQUIRE(fixture.kernel.capabilities().replace(fixture.session, grants));
        REQUIRE(fixture.kernel.bootstrap());
        CHECK(fixture.kernel.commandRegistry().frozen());
        const auto catalog = fixture.kernel.execution().catalog();
        const auto found = std::find_if(catalog.commands.begin(), catalog.commands.end(), [&](const auto& d) {
            return d.name == descriptor.name;
        });
        REQUIRE(found != catalog.commands.end());
        CHECK(found->lifecycleOperation == LifecycleOperation::DocumentClose);
        CHECK(found->scope == ExecutionScope::Document);
        CHECK(found->capability == descriptor.capability);
        CHECK_FALSE(lasercnc::test::installKernelTestModule(fixture.kernel, [&](auto& builder) {
            builder.lifecycleCommand(descriptor);
        }));
        auto request = lifecycleRequest(fixture, "kernel.document.close");
        CHECK_FALSE(fixture.kernel.execution().executeCommand(request));
        request.versionResolution = VersionResolution::Compatible;
        auto result = fixture.kernel.execution().executeCommand(request);
        REQUIRE(result);
        CHECK(result.value().resolvedVersion == descriptor.version);
        CHECK(result.value().contractStatus == ContractStatus::Deprecated);
        REQUIRE(fixture.kernel.shutdown());
    }
}

TEST_CASE("Lifecycle result schema failure cannot turn a completed transition into a retryable failure", "[runtime][command][lifecycle-control]")
{
    class ResultFaultValidator final : public ISchemaValidator {
    public:
        explicit ResultFaultValidator(bool throws) : throws_(throws) {}
        Result<void> validate(const Schema& expected, const Value&) const override {
            if(expected.id() == validId<SchemaId>("schema.command.result.object")) {
                if(throws_) { throw std::runtime_error("Lifecycle result validation fault"); }
                return Result<void>::failure(makeError("Test.ResultRejected", ErrorCategory::Validation, "Injected failure"));
            }
            return Result<void>::success();
        }
    private:
        bool throws_;
    };
    for(bool throws : {false, true}) {
        RuntimeFixture fixture;
        installLifecycleCommands(fixture);
        REQUIRE(fixture.kernel.executionServices().configure(std::make_shared<ResultFaultValidator>(throws), fixture.log));
        REQUIRE(fixture.kernel.bootstrap());
        auto result = fixture.kernel.execution().executeCommand(lifecycleRequest(fixture, "kernel.document.close"));
        REQUIRE(result);
        REQUIRE(result.value().postExecutionErrors.size() == 1U);
        CHECK(std::string(result.value().postExecutionErrors.front().code.value())
            == (throws ? "Command.PostExecutionIntegrationFailed" : "Test.ResultRejected"));
        CHECK(fixture.kernel.documentRuntime().lifecycle(fixture.document).value().state == DocumentLifecycleState::Detached);
        REQUIRE(fixture.kernel.shutdown());
    }
}

TEST_CASE("Command and query requests resolve compatible deprecated contracts explicitly", "[runtime][command][query][version]")
{
    RuntimeFixture fixture;
    fixture.create = std::make_shared<CreateObjectHandler>();
    fixture.query = std::make_shared<ObjectQueryHandler>();

    auto commandOne = commandDescriptor("kernel.versioned.create");
    auto commandOneTwo = commandOne;
    commandOneTwo.version = Version {1U, 2U, 0U};
    commandOneTwo.status = ContractStatus::Deprecated;
    REQUIRE(lasercnc::test::registerCommand(fixture.kernel,
        commandOne, fixture.create).hasValue());
    REQUIRE(lasercnc::test::registerCommand(fixture.kernel,
        commandOneTwo, fixture.create).hasValue());

    auto queryOne = queryDescriptor("kernel.versioned.get");
    auto queryOneOne = queryOne;
    queryOneOne.version = Version {1U, 1U, 0U};
    queryOneOne.status = ContractStatus::Deprecated;
    REQUIRE(lasercnc::test::registerQuery(fixture.kernel,
        queryOne, fixture.query).hasValue());
    REQUIRE(lasercnc::test::registerQuery(fixture.kernel,
        queryOneOne, fixture.query).hasValue());
    REQUIRE(fixture.kernel.bootstrap().hasValue());

    auto unsupported = commandRequest(
        "request.version.unsupported",
        fixture.project,
        fixture.document,
        fixture.session,
        "kernel.versioned.create",
        "object.version.unsupported");
    unsupported.version = Version {1U, 3U, 0U};
    auto unsupportedResult = fixture.kernel.execution().executeCommand(unsupported);
    REQUIRE_FALSE(unsupportedResult.hasValue());
    CHECK(std::string(unsupportedResult.error().code.value()) == "Command.UnsupportedVersion");
    CHECK(fixture.create->calls == 0U);

    auto compatible = commandRequest(
        "request.version.compatible",
        fixture.project,
        fixture.document,
        fixture.session,
        "kernel.versioned.create",
        "object.versioned");
    compatible.versionResolution = VersionResolution::Compatible;
    auto command = fixture.kernel.execution().executeCommand(compatible);
    REQUIRE(command.hasValue());
    CHECK(command.value().resolvedVersion == Version {1U, 2U, 0U});
    CHECK(command.value().contractStatus == ContractStatus::Deprecated);

    auto queryRequestValue = queryRequest(
        fixture.project, fixture.document, fixture.session, "object.versioned");
    queryRequestValue.query = validId<QueryName>("kernel.versioned.get");
    queryRequestValue.versionResolution = VersionResolution::Compatible;
    auto query = fixture.kernel.execution().executeQuery(queryRequestValue);
    REQUIRE(query.hasValue());
    CHECK(query.value().resolvedVersion == Version {1U, 1U, 0U});
    CHECK(query.value().contractStatus == ContractStatus::Deprecated);
    CHECK(fixture.create->calls == 1U);
    CHECK(fixture.query->calls == 1U);

    REQUIRE(fixture.kernel.shutdown().hasValue());
}

TEST_CASE("Command and query requests reject unknown version resolution without execution",
          "[runtime][command][query][version][c6b15]")
{
    RuntimeFixture fixture;
    fixture.registerStandardHandlers();
    REQUIRE(fixture.kernel.bootstrap().hasValue());

    auto command = commandRequest(
        "request.invalid-command-resolution",
        fixture.project,
        fixture.document,
        fixture.session,
        "kernel.object.create",
        "object.invalid-resolution");
    command.versionResolution = static_cast<VersionResolution>(255U);
    const auto commandResult = fixture.kernel.execution().executeCommand(command);
    REQUIRE_FALSE(commandResult.hasValue());
    CHECK(std::string(commandResult.error().code.value())
          == "Command.InvalidVersionResolution");
    CHECK(fixture.create->calls == 0U);

    auto query = queryRequest(
        fixture.project, fixture.document, fixture.session, "object.invalid-resolution");
    query.versionResolution = static_cast<VersionResolution>(255U);
    const auto queryResult = fixture.kernel.execution().executeQuery(query);
    REQUIRE_FALSE(queryResult.hasValue());
    CHECK(std::string(queryResult.error().code.value())
          == "Query.InvalidVersionResolution");
    CHECK(fixture.query->calls == 0U);

    const auto spans = fixture.kernel.traces().records();
    REQUIRE(spans.size() == 2U);
    for(const auto& span : spans) {
        const auto found = span.attributes.find("versionResolution");
        REQUIRE(found != span.attributes.end());
        const auto* value = found->second.getIf<std::string>();
        REQUIRE(value != nullptr);
        CHECK(*value == "unknown");
        CHECK(span.status == TraceStatus::Failed);
    }

    REQUIRE(fixture.kernel.shutdown().hasValue());
}

TEST_CASE("QueryRuntime enforces global session project and document scopes", "[runtime][query][scope]")
{
    RuntimeFixture fixture;
    auto handler = std::make_shared<ScopeQueryHandler>();
    const std::array scopes {
        std::pair {"kernel.scope.global", ExecutionScope::Global},
        std::pair {"kernel.scope.session", ExecutionScope::Session},
        std::pair {"kernel.scope.project", ExecutionScope::Project},
        std::pair {"kernel.scope.document", ExecutionScope::Document}};
    for(const auto& [name, scope] : scopes) {
        REQUIRE(lasercnc::test::registerQuery(fixture.kernel,
            queryDescriptor(name, scope), handler).hasValue());
    }
    REQUIRE(fixture.kernel.bootstrap().hasValue());

    const auto before = fixture.kernel.documents().snapshot(fixture.document);
    REQUIRE(before.hasValue());

    auto execute = [&](const char* requestId, const char* query, ExecutionContext context) {
        const bool expectsDocument = context.documentId.has_value();
        auto response = fixture.kernel.execution().executeQuery(QueryRequest {
            validId<RequestId>(requestId),
            std::move(context),
            validId<QueryName>(query),
            Version {1U, 0U, 0U},
            Value {Value::Object {}},
            validId<CorrelationId>("correlation.scope"),
            validId<TraceId>("trace.scope")});
        if(response) {
            const auto* fields = response.value().result.getIf<Value::Object>();
            REQUIRE(fields != nullptr);
            REQUIRE(fields->contains("hasDocument"));
            const auto* hasDocument = fields->at("hasDocument").getIf<bool>();
            REQUIRE(hasDocument != nullptr);
            CHECK(*hasDocument == expectsDocument);
            CHECK(response.value().revisions.has_value() == expectsDocument);
        }
        return response;
    };

    CHECK(execute(
        "request.scope.global",
        "kernel.scope.global",
        ExecutionContext {fixture.session, std::nullopt, std::nullopt}).hasValue());
    CHECK(execute(
        "request.scope.session",
        "kernel.scope.session",
        ExecutionContext {fixture.session, std::nullopt, std::nullopt}).hasValue());
    CHECK(execute(
        "request.scope.project",
        "kernel.scope.project",
        ExecutionContext {fixture.session, fixture.project, std::nullopt}).hasValue());
    CHECK(execute(
        "request.scope.document",
        "kernel.scope.document",
        ExecutionContext {fixture.session, fixture.project, fixture.document}).hasValue());
    CHECK(handler->calls == 4U);

    auto mismatched = execute(
        "request.scope.mismatch",
        "kernel.scope.global",
        ExecutionContext {fixture.session, fixture.project, std::nullopt});
    REQUIRE_FALSE(mismatched.hasValue());
    CHECK(std::string(mismatched.error().code.value()) == "Query.ScopeMismatch");
    CHECK(handler->calls == 4U);

    const auto after = fixture.kernel.documents().snapshot(fixture.document);
    REQUIRE(after.hasValue());
    CHECK(after.value().revisions() == before.value().revisions());
    CHECK(after.value().objects().all() == before.value().objects().all());
    REQUIRE(fixture.kernel.shutdown().hasValue());
}

TEST_CASE("Synchronous read-only commands preserve every execution scope", "[runtime][command][scope]")
{
    RuntimeFixture fixture;
    auto handler = std::make_shared<ScopeCommandHandler>();
    const std::array scopes {
        std::pair {"kernel.command-scope.global", ExecutionScope::Global},
        std::pair {"kernel.command-scope.session", ExecutionScope::Session},
        std::pair {"kernel.command-scope.project", ExecutionScope::Project},
        std::pair {"kernel.command-scope.document", ExecutionScope::Document}};
    for(const auto& [name, scope] : scopes) {
        auto descriptor = commandDescriptor(name);
        descriptor.sideEffect = SideEffectLevel::ReadOnly;
        descriptor.capability = validId<CapabilityId>("document.read");
        descriptor.idempotent = false;
        descriptor.scope = scope;
        REQUIRE(lasercnc::test::registerReadOnlyCommand(fixture.kernel,
            descriptor, handler).hasValue());
    }
    REQUIRE(fixture.kernel.bootstrap().hasValue());

    const auto before = fixture.kernel.documents().snapshot(fixture.document);
    REQUIRE(before.hasValue());

    auto execute = [&](const char* requestId, const char* command, ExecutionContext context) {
        const bool expectsDocument = context.documentId.has_value();
        auto response = fixture.kernel.execution().executeCommand(CommandRequest {
            validId<RequestId>(requestId),
            std::move(context),
            validId<CommandName>(command),
            Version {1U, 0U, 0U},
            Value {Value::Object {}},
            std::nullopt,
            validId<CorrelationId>("correlation.command-scope"),
            validId<TraceId>("trace.command-scope")});
        if(response) {
            const auto* fields = response.value().result.getIf<Value::Object>();
            REQUIRE(fields != nullptr);
            REQUIRE(fields->contains("hasDocument"));
            const auto* hasDocument = fields->at("hasDocument").getIf<bool>();
            REQUIRE(hasDocument != nullptr);
            CHECK(*hasDocument == expectsDocument);
            CHECK_FALSE(response.value().commit.has_value());
            CHECK_FALSE(response.value().taskId.has_value());
        }
        return response;
    };

    auto global = execute(
        "request.command-scope.global",
        "kernel.command-scope.global",
        ExecutionContext {fixture.session, std::nullopt, std::nullopt});
    REQUIRE(global.hasValue());
    CHECK_FALSE(global.value().commit.has_value());
    CHECK(execute(
        "request.command-scope.session",
        "kernel.command-scope.session",
        ExecutionContext {fixture.session, std::nullopt, std::nullopt}).hasValue());
    CHECK(execute(
        "request.command-scope.project",
        "kernel.command-scope.project",
        ExecutionContext {fixture.session, fixture.project, std::nullopt}).hasValue());
    CHECK(execute(
        "request.command-scope.document",
        "kernel.command-scope.document",
        ExecutionContext {fixture.session, fixture.project, fixture.document}).hasValue());
    CHECK(handler->calls == 4U);

    auto mismatch = execute(
        "request.command-scope.mismatch",
        "kernel.command-scope.project",
        ExecutionContext {fixture.session, fixture.project, fixture.document});
    REQUIRE_FALSE(mismatch.hasValue());
    CHECK(std::string(mismatch.error().code.value()) == "Command.ScopeMismatch");
    CHECK(handler->calls == 4U);

    const auto after = fixture.kernel.documents().snapshot(fixture.document);
    REQUIRE(after.hasValue());
    CHECK(after.value().revisions() == before.value().revisions());
    CHECK(after.value().objects().all() == before.value().objects().all());
    REQUIRE(fixture.kernel.shutdown().hasValue());
}

TEST_CASE("CommandRuntime enforces schema capability project and revision before writes", "[runtime][command]")
{
    RuntimeFixture fixture;
    fixture.registerStandardHandlers();
    REQUIRE(fixture.kernel.bootstrap().hasValue());

    auto invalidScope = commandRequest(
        "request.invalid-scope", fixture.project, fixture.document, fixture.session,
        "kernel.object.create", "object.invalid-scope");
    invalidScope.context.documentId.reset();
    auto invalidScopeResult = fixture.kernel.execution().executeCommand(invalidScope);
    REQUIRE_FALSE(invalidScopeResult.hasValue());
    CHECK(std::string(invalidScopeResult.error().code.value()) == "Command.ScopeMismatch");
    CHECK(fixture.create->calls == 0U);

    auto invalidSchema = commandRequest(
        "request.invalid-schema", fixture.project, fixture.document, fixture.session,
        "kernel.object.create", "object.invalid-schema");
    invalidSchema.arguments = Value {"not-an-object"};
    CHECK_FALSE(fixture.kernel.execution().executeCommand(invalidSchema).hasValue());
    CHECK(fixture.create->calls == 0U);

    const std::array<CapabilityId, 0U> noCapabilities {};
    REQUIRE(fixture.kernel.capabilities().replace(fixture.session, noCapabilities).hasValue());
    auto denied = fixture.kernel.execution().executeCommand(commandRequest(
        "request.denied", fixture.project, fixture.document, fixture.session,
        "kernel.object.create", "object.denied"));
    REQUIRE_FALSE(denied.hasValue());
    CHECK(std::string(denied.error().code.value()) == "Capability.Denied");
    CHECK(fixture.create->calls == 0U);

    const std::array grants {validId<CapabilityId>("document.write")};
    REQUIRE(fixture.kernel.capabilities().replace(fixture.session, grants).hasValue());
    auto wrongProject = fixture.kernel.execution().executeCommand(commandRequest(
        "request.wrong-project", validId<ProjectId>("project.other"), fixture.document,
        fixture.session, "kernel.object.create", "object.wrong-project"));
    REQUIRE_FALSE(wrongProject.hasValue());
    CHECK(std::string(wrongProject.error().code.value()) == "Command.ProjectMismatch");

    auto first = fixture.kernel.execution().executeCommand(commandRequest(
        "request.first", fixture.project, fixture.document, fixture.session,
        "kernel.object.create", "object.first"));
    REQUIRE(first.hasValue());
    auto stale = fixture.kernel.execution().executeCommand(commandRequest(
        "request.stale", fixture.project, fixture.document, fixture.session,
        "kernel.object.create", "object.stale", std::nullopt, Revision {0U}));
    REQUIRE_FALSE(stale.hasValue());
    CHECK(std::string(stale.error().code.value()) == "Project.RevisionConflict");
    auto snapshot = fixture.kernel.documents().snapshot(fixture.document);
    REQUIRE(snapshot.hasValue());
    CHECK_FALSE(snapshot.value().objects().contains(validId<ObjectId>("object.stale")));
}

TEST_CASE("CommandRuntime rolls back handler failures exceptions and result schema errors", "[runtime][command]")
{
    RuntimeFixture fixture;
    auto failing = std::make_shared<FailingHandler>(false);
    auto throwing = std::make_shared<FailingHandler>(true);
    REQUIRE(lasercnc::test::registerCommand(fixture.kernel,
        commandDescriptor("kernel.fail"), failing).hasValue());
    REQUIRE(lasercnc::test::registerCommand(fixture.kernel,
        commandDescriptor("kernel.throw"), throwing).hasValue());
    auto badResult = std::make_shared<CreateObjectHandler>();
    auto badDescriptor = commandDescriptor("kernel.bad-result");
    badDescriptor.result = schema("schema.command.result.string", SchemaKind::String);
    REQUIRE(lasercnc::test::registerCommand(fixture.kernel,
        std::move(badDescriptor), badResult).hasValue());
    REQUIRE(fixture.kernel.bootstrap().hasValue());

    auto failed = fixture.kernel.execution().executeCommand(commandRequest(
        "request.fail", fixture.project, fixture.document, fixture.session,
        "kernel.fail", "object.ignored"));
    REQUIRE_FALSE(failed.hasValue());
    CHECK(std::string(failed.error().code.value()) == "Test.HandlerRejected");

    auto threw = fixture.kernel.execution().executeCommand(commandRequest(
        "request.throw", fixture.project, fixture.document, fixture.session,
        "kernel.throw", "object.ignored"));
    REQUIRE_FALSE(threw.hasValue());
    CHECK(std::string(threw.error().code.value()) == "Command.HandlerFailed");

    auto invalidResult = fixture.kernel.execution().executeCommand(commandRequest(
        "request.bad-result", fixture.project, fixture.document, fixture.session,
        "kernel.bad-result", "object.bad-result"));
    REQUIRE_FALSE(invalidResult.hasValue());
    CHECK(std::string(invalidResult.error().code.value()) == "Runtime.SchemaInvalid");

    auto snapshot = fixture.kernel.documents().snapshot(fixture.document);
    REQUIRE(snapshot.hasValue());
    CHECK(snapshot.value().objects().empty());
    CHECK(snapshot.value().revisions().at(RevisionScope::Project) == Revision {0U});
}

TEST_CASE("CommandRuntime idempotency replays one commit and rejects key rebinding", "[runtime][command]")
{
    RuntimeFixture fixture;
    fixture.registerStandardHandlers();
    std::size_t eventCount = 0U;
    auto subscription = fixture.kernel.events().subscribe(
        validId<SubscriptionId>("subscription.idempotency"),
        EventFilter {EventKind::Domain, std::nullopt},
        DeliveryMode::Immediate,
        [&](const EventEnvelope&) { ++eventCount; });
    REQUIRE(subscription.hasValue());
    REQUIRE(fixture.kernel.bootstrap().hasValue());

    const auto key = validId<IdempotencyKey>("idempotency.create-object");
    auto first = fixture.kernel.execution().executeCommand(commandRequest(
        "request.idempotency.first", fixture.project, fixture.document, fixture.session,
        "kernel.object.create", "object.idempotent", key));
    REQUIRE(first.hasValue());
    CHECK_FALSE(first.value().replayed);

    auto replay = fixture.kernel.execution().executeCommand(commandRequest(
        "request.idempotency.retry", fixture.project, fixture.document, fixture.session,
        "kernel.object.create", "object.idempotent", key));
    REQUIRE(replay.hasValue());
    CHECK(replay.value().replayed);
    REQUIRE(replay.value().commit.has_value());
    REQUIRE(first.value().commit.has_value());
    CHECK(replay.value().commit->transactionId == first.value().commit->transactionId);
    CHECK(fixture.create->calls == 1U);
    CHECK(eventCount == 1U);

    auto resolutionChanged = commandRequest(
        "request.idempotency.resolution-conflict",
        fixture.project,
        fixture.document,
        fixture.session,
        "kernel.object.create",
        "object.idempotent",
        key);
    resolutionChanged.versionResolution = VersionResolution::Compatible;
    auto resolutionConflict = fixture.kernel.execution().executeCommand(resolutionChanged);
    REQUIRE_FALSE(resolutionConflict.hasValue());
    CHECK(std::string(resolutionConflict.error().code.value())
          == "Command.IdempotencyKeyConflict");
    CHECK(fixture.create->calls == 1U);

    auto rebound = fixture.kernel.execution().executeCommand(commandRequest(
        "request.idempotency.conflict", fixture.project, fixture.document, fixture.session,
        "kernel.object.create", "object.different", key));
    REQUIRE_FALSE(rebound.hasValue());
    CHECK(std::string(rebound.error().code.value()) == "Command.IdempotencyKeyConflict");
}

TEST_CASE("Concurrent duplicate idempotency requests share one in-flight execution", "[runtime][command]")
{
    RuntimeFixture fixture;
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::promise<void> release;
    auto handler = std::make_shared<SlowCreateHandler>(entered, release.get_future().share());
    REQUIRE(lasercnc::test::registerCommand(fixture.kernel,
        commandDescriptor("kernel.slow-create"), handler).hasValue());
    REQUIRE(fixture.kernel.bootstrap().hasValue());
    const auto key = validId<IdempotencyKey>("idempotency.concurrent");

    auto first = std::async(std::launch::async, [&]() {
        return fixture.kernel.execution().executeCommand(commandRequest(
            "request.concurrent.first", fixture.project, fixture.document, fixture.session,
            "kernel.slow-create", "unused", key));
    });
    enteredFuture.wait();
    auto refused = fixture.kernel.shutdown();
    REQUIRE_FALSE(refused.hasValue());
    CHECK(std::string(refused.error().code.value()) == "Kernel.ActiveTransactions");
    CHECK(fixture.kernel.state() == lasercnc::kernel::AppKernelState::Ready);
    auto second = std::async(std::launch::async, [&]() {
        return fixture.kernel.execution().executeCommand(commandRequest(
            "request.concurrent.second", fixture.project, fixture.document, fixture.session,
            "kernel.slow-create", "unused", key));
    });
    release.set_value();

    auto firstResult = first.get();
    auto secondResult = second.get();
    REQUIRE(firstResult.hasValue());
    REQUIRE(secondResult.hasValue());
    CHECK_FALSE(firstResult.value().replayed);
    CHECK(secondResult.value().replayed);
    CHECK(handler->calls == 1U);
}

TEST_CASE("Post-commit event and logging failures cannot invert command success", "[runtime][command]")
{
    RuntimeFixture fixture;
    fixture.registerStandardHandlers();
    auto throwing = fixture.kernel.events().subscribe(
        validId<SubscriptionId>("subscription.post-commit-failure"),
        EventFilter {EventKind::Domain, std::nullopt},
        DeliveryMode::Immediate,
        [](const EventEnvelope&) { throw std::runtime_error("event failure"); });
    REQUIRE(throwing.hasValue());
    REQUIRE(fixture.kernel.bootstrap().hasValue());
    fixture.log->failWrites = true;

    auto command = fixture.kernel.execution().executeCommand(commandRequest(
        "request.post-commit", fixture.project, fixture.document, fixture.session,
        "kernel.object.create", "object.post-commit"));
    REQUIRE(command.hasValue());
    REQUIRE(command.value().postExecutionErrors.size() == 2U);
    CHECK(std::string(command.value().postExecutionErrors[0].code.value())
          == "Event.SubscriberFailed");
    CHECK(std::string(command.value().postExecutionErrors[1].code.value()) == "Test.LogFailed");
    auto snapshot = fixture.kernel.documents().snapshot(fixture.document);
    REQUIRE(snapshot.hasValue());
    CHECK(snapshot.value().objects().contains(validId<ObjectId>("object.post-commit")));
}

TEST_CASE("QueryRuntime requires capability and an immutable owned document", "[runtime][query]")
{
    RuntimeFixture fixture;
    fixture.registerStandardHandlers();
    REQUIRE(fixture.kernel.bootstrap().hasValue());

    auto missingDocument = queryRequest(
        fixture.project, fixture.document, fixture.session, "object.missing");
    missingDocument.context.documentId.reset();
    auto missing = fixture.kernel.execution().executeQuery(missingDocument);
    REQUIRE_FALSE(missing.hasValue());
    CHECK(std::string(missing.error().code.value()) == "Query.ScopeMismatch");

    const std::array<CapabilityId, 0U> noCapabilities {};
    REQUIRE(fixture.kernel.capabilities().replace(fixture.session, noCapabilities).hasValue());
    auto denied = fixture.kernel.execution().executeQuery(queryRequest(
        fixture.project, fixture.document, fixture.session, "object.missing"));
    REQUIRE_FALSE(denied.hasValue());
    CHECK(std::string(denied.error().code.value()) == "Capability.Denied");
    CHECK(fixture.query->calls == 0U);
}

TEST_CASE("AppKernel refuses registered runtimes without execution services", "[kernel][runtime]")
{
    lasercnc::kernel::AppKernel kernel;
    REQUIRE(lasercnc::test::registerCommand(kernel,
        commandDescriptor("kernel.unconfigured"),
        std::make_shared<CreateObjectHandler>()).hasValue());
    auto bootstrapped = kernel.bootstrap();
    REQUIRE_FALSE(bootstrapped.hasValue());
    CHECK(std::string(bootstrapped.error().code.value())
          == "Runtime.ExecutionServicesNotConfigured");
    CHECK(kernel.state() == lasercnc::kernel::AppKernelState::Failed);
}

TEST_CASE("Project only execution rejects unavailable containers before invoking handlers", "[kernel][runtime][project-admission]")
{
    for(const bool command : {false, true}) {
        RuntimeFixture fixture;
        const auto project = validId<ProjectId>("project.empty-admission");
        REQUIRE(fixture.kernel.addProject(project));
        auto handler = std::make_shared<ProjectActivityProbe>();
        auto descriptor = commandDescriptor("kernel.project.probe");
        descriptor.scope = ExecutionScope::Project;
        descriptor.sideEffect = SideEffectLevel::ReadOnly;
        descriptor.idempotent = false;
        REQUIRE(lasercnc::test::registerReadOnlyCommand(fixture.kernel, descriptor, handler));
        REQUIRE(lasercnc::test::registerQuery(fixture.kernel, queryDescriptor("kernel.project.probe", ExecutionScope::Project), handler));
        REQUIRE(fixture.kernel.bootstrap());
        REQUIRE(fixture.kernel.projectRuntime().close(project));
        for(const auto& unavailable : {project, validId<ProjectId>("project.missing-admission")}) {
            if(command) {
                auto request = commandRequest("request.project-probe", unavailable, fixture.document, fixture.session,
                    "kernel.project.probe", "unused");
                request.context.documentId.reset();
                const auto rejected = fixture.kernel.execution().executeCommand(request);
                CHECK_FALSE(rejected);
                if(!rejected) { CHECK(std::string(rejected.error().code.value()) == "Project.NotOpen"); }
            } else {
                auto request = queryRequest(unavailable, fixture.document, fixture.session, "unused");
                request.context.documentId.reset();
                request.query = validId<QueryName>("kernel.project.probe");
                const auto rejected = fixture.kernel.execution().executeQuery(request);
                CHECK_FALSE(rejected);
                if(!rejected) { CHECK(std::string(rejected.error().code.value()) == "Project.NotOpen"); }
            }
        }
        CHECK(handler->calls == 0U);
        REQUIRE(fixture.kernel.projectRuntime().open(project));
        REQUIRE(fixture.kernel.shutdown());
    }
}

TEST_CASE("Project only execution retains its container through handler and trace publication", "[kernel][runtime][project-admission]")
{
    for(const bool command : {false, true}) {
        RuntimeFixture fixture;
        const auto project = validId<ProjectId>("project.empty-probe");
        REQUIRE(fixture.kernel.addProject(project));
        auto handler = std::make_shared<ProjectActivityProbe>();
        auto exporter = std::make_shared<ProjectTraceProbe>();
        REQUIRE(fixture.kernel.traces().addExporter(exporter));
        auto descriptor = commandDescriptor("kernel.project.probe");
        descriptor.scope = ExecutionScope::Project;
        descriptor.sideEffect = SideEffectLevel::ReadOnly;
        descriptor.idempotent = false;
        REQUIRE(lasercnc::test::registerReadOnlyCommand(fixture.kernel, descriptor, handler));
        REQUIRE(lasercnc::test::registerQuery(fixture.kernel, queryDescriptor("kernel.project.probe", ExecutionScope::Project), handler));
        REQUIRE(fixture.kernel.bootstrap());
        std::optional<Result<ProjectLifecycleSnapshot>> duringHandler;
        std::optional<Result<ProjectLifecycleSnapshot>> duringExport;
        handler->probe = [&]() { duringHandler.emplace(fixture.kernel.projectRuntime().close(project)); };
        exporter->probe = [&]() { duringExport.emplace(fixture.kernel.projectRuntime().close(project)); };
        if(command) {
            auto request = commandRequest("request.project-probe", project, fixture.document, fixture.session,
                "kernel.project.probe", "unused");
            request.context.documentId.reset();
            REQUIRE(fixture.kernel.execution().executeCommand(request));
        } else {
            auto request = queryRequest(project, fixture.document, fixture.session, "unused");
            request.context.documentId.reset();
            request.query = validId<QueryName>("kernel.project.probe");
            REQUIRE(fixture.kernel.execution().executeQuery(request));
        }
        for(const auto* result : {&duringHandler, &duringExport}) {
            REQUIRE(result->has_value());
            CHECK_FALSE(result->value());
            if(!result->value()) { CHECK(std::string(result->value().error().code.value()) == "Project.CloseBlocked"); }
        }
        CHECK(fixture.kernel.projectRuntime().lifecycle(project).value().state == ProjectLifecycleState::Open);
        CHECK(fixture.kernel.projectRuntime().lifecycle(project).value().activities == 0U);
        REQUIRE(fixture.kernel.projectRuntime().close(fixture.project));
        if(fixture.kernel.projectRuntime().lifecycle(project).value().state == ProjectLifecycleState::Open) {
            REQUIRE(fixture.kernel.projectRuntime().close(project));
        }
        REQUIRE(fixture.kernel.shutdown());
    }
}

TEST_CASE("Project only execution releases activity on validation and handler failures", "[kernel][runtime][project-admission]")
{
    for(const bool command : {false, true}) {
        for(const bool throws : {false, true}) {
            RuntimeFixture fixture;
            const auto project = validId<ProjectId>("project.failure-probe");
            REQUIRE(fixture.kernel.addProject(project));
            auto handler = std::make_shared<ProjectActivityProbe>();
            auto descriptor = commandDescriptor("kernel.project.failure");
            descriptor.scope = ExecutionScope::Project;
            descriptor.sideEffect = SideEffectLevel::ReadOnly;
            descriptor.idempotent = false;
            REQUIRE(lasercnc::test::registerReadOnlyCommand(fixture.kernel, descriptor, handler));
            REQUIRE(lasercnc::test::registerQuery(fixture.kernel,
                queryDescriptor("kernel.project.failure", ExecutionScope::Project), handler));
            REQUIRE(fixture.kernel.bootstrap());
            handler->probe = []() { throw std::runtime_error("Injected project handler failure"); };
            if(command) {
                auto request = commandRequest("request.project-failure", project, fixture.document,
                    fixture.session, "kernel.project.failure", "unused");
                request.context.documentId.reset();
                if(!throws) { request.arguments = Value{"invalid object"}; }
                CHECK_FALSE(fixture.kernel.execution().executeCommand(request));
            } else {
                auto request = queryRequest(project, fixture.document, fixture.session, "unused");
                request.context.documentId.reset();
                request.query = validId<QueryName>("kernel.project.failure");
                if(!throws) { request.arguments = Value{"invalid object"}; }
                CHECK_FALSE(fixture.kernel.execution().executeQuery(request));
            }
            CHECK(handler->calls == (throws ? 1U : 0U));
            CHECK(fixture.kernel.projectRuntime().lifecycle(project).value().activities == 0U);
            REQUIRE(fixture.kernel.projectRuntime().close(project));
            REQUIRE(fixture.kernel.shutdown());
        }
    }
}

TEST_CASE("AppKernel refuses shutdown while a query execution is active", "[kernel][runtime][query]")
{
    RuntimeFixture fixture;
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::promise<void> release;
    auto handler = std::make_shared<BlockingQueryHandler>(
        entered, release.get_future().share());
    REQUIRE(lasercnc::test::registerQuery(fixture.kernel,
        queryDescriptor("kernel.blocking-query", ExecutionScope::Session), handler).hasValue());
    REQUIRE(fixture.kernel.bootstrap().hasValue());

    auto running = std::async(std::launch::async, [&]() {
        return fixture.kernel.execution().executeQuery(QueryRequest {
            validId<RequestId>("request.blocking-query"),
            ExecutionContext {fixture.session, std::nullopt, std::nullopt},
            validId<QueryName>("kernel.blocking-query"),
            Version {1U, 0U, 0U},
            Value {Value::Object {}},
            validId<CorrelationId>("correlation.blocking-query"),
            validId<TraceId>("trace.blocking-query")});
    });
    enteredFuture.wait();
    auto refused = fixture.kernel.shutdown();
    REQUIRE_FALSE(refused.hasValue());
    CHECK(std::string(refused.error().code.value()) == "Kernel.ActiveExecutions");
    CHECK(fixture.kernel.state() == lasercnc::kernel::AppKernelState::Ready);

    release.set_value();
    REQUIRE(running.get().hasValue());
    REQUIRE(fixture.kernel.shutdown().hasValue());
}

TEST_CASE("Kernel admission retains all query scopes through trace publication", "[kernel][runtime][kernel-admission]")
{
    for(const auto scope : {ExecutionScope::Global, ExecutionScope::Session, ExecutionScope::Project, ExecutionScope::Document}) {
        RuntimeFixture fixture;
        auto exporter = std::make_shared<ShutdownTraceExporter>(fixture.kernel);
        REQUIRE(fixture.kernel.traces().addExporter(exporter));
        auto handler = std::make_shared<ScopeQueryHandler>();
        REQUIRE(lasercnc::test::registerQuery(fixture.kernel, queryDescriptor("kernel.scope.admission", scope), handler));
        REQUIRE(fixture.kernel.bootstrap());
        auto request = queryRequest(fixture.project, fixture.document, fixture.session, "unused");
        request.query = validId<QueryName>("kernel.scope.admission");
        if(scope != ExecutionScope::Document) { request.context.documentId.reset(); }
        if(scope == ExecutionScope::Global || scope == ExecutionScope::Session) { request.context.projectId.reset(); }
        REQUIRE(fixture.kernel.execution().executeQuery(request));
        CHECK(handler->calls == 1U);
        REQUIRE(exporter->stopped.has_value());
        CHECK_FALSE(exporter->stopped->hasValue());
        if(!exporter->stopped->hasValue()) { CHECK(std::string(exporter->stopped->error().code.value()) == "Kernel.ActiveExecutions"); }
        CHECK(fixture.kernel.state() == AppKernelState::Ready);
        REQUIRE(fixture.kernel.shutdown());
    }
}

TEST_CASE("Kernel admission retains all command scopes through trace publication", "[kernel][runtime][kernel-admission]")
{
    for(const auto scope : {ExecutionScope::Global, ExecutionScope::Session, ExecutionScope::Project, ExecutionScope::Document}) {
        RuntimeFixture fixture;
        auto exporter = std::make_shared<ShutdownTraceExporter>(fixture.kernel);
        REQUIRE(fixture.kernel.traces().addExporter(exporter));
        auto handler = std::make_shared<ScopeCommandHandler>();
        auto descriptor = commandDescriptor("kernel.scope.admission");
        descriptor.sideEffect = SideEffectLevel::ReadOnly;
        descriptor.idempotent = false;
        descriptor.scope = scope;
        REQUIRE(lasercnc::test::registerReadOnlyCommand(fixture.kernel, descriptor, handler));
        REQUIRE(fixture.kernel.bootstrap());
        auto request = commandRequest("request.admission", fixture.project, fixture.document, fixture.session,
            "kernel.scope.admission", "unused");
        if(scope != ExecutionScope::Document) { request.context.documentId.reset(); }
        if(scope == ExecutionScope::Global || scope == ExecutionScope::Session) { request.context.projectId.reset(); }
        REQUIRE(fixture.kernel.execution().executeCommand(request));
        CHECK(handler->calls == 1U);
        REQUIRE(exporter->stopped.has_value());
        CHECK_FALSE(exporter->stopped->hasValue());
        if(!exporter->stopped->hasValue()) { CHECK(std::string(exporter->stopped->error().code.value()) == "Kernel.ActiveExecutions"); }
        CHECK(fixture.kernel.state() == AppKernelState::Ready);
        REQUIRE(fixture.kernel.shutdown());
    }
}

TEST_CASE("Kernel admission closes before module stop and rejects concurrent lifecycle calls", "[kernel][runtime][kernel-admission]")
{
    RuntimeFixture fixture;
    auto handler = std::make_shared<ScopeQueryHandler>();
    REQUIRE(lasercnc::test::registerQuery(fixture.kernel, queryDescriptor("kernel.scope.stop", ExecutionScope::Global), handler));
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::promise<void> release;
    auto releaseFuture = release.get_future().share();
    std::optional<Result<void>> reentered;
    unsigned int stops = 0U;
    REQUIRE(fixture.kernel.addModule(std::make_unique<AdmissionStopModule>([&](AppKernel& kernel) {
        ++stops;
        reentered.emplace(kernel.shutdown());
        entered.set_value();
        releaseFuture.wait();
        return Result<void>::success();
    })));
    REQUIRE(fixture.kernel.bootstrap());
    auto stopping = std::async(std::launch::async, [&]() { return fixture.kernel.shutdown(); });
    // Always release the worker before REQUIRE, including a broken stop implementation.
    // 中文翻译：包括停止实现出错在内，任何致命断言之前都必须释放工作线程。
    const bool reached = enteredFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    bool secondDenied = false;
    bool requestsDenied = false;
    if(reached) {
        auto second = fixture.kernel.shutdown();
        secondDenied = !second && second.error().code.value() == "Kernel.LifecycleInProgress";
        auto request = queryRequest(fixture.project, fixture.document, fixture.session, "unused");
        request.query = validId<QueryName>("kernel.scope.stop");
        request.context.projectId.reset();
        request.context.documentId.reset();
        auto rejected = fixture.kernel.execution().executeQuery(request);
        requestsDenied = !rejected && rejected.error().code.value() == "Query.RuntimeNotReady"
            && !fixture.kernel.projectRuntime().create(validId<ProjectId>("project.after-stop"))
            && !fixture.kernel.documentRuntime().create(fixture.project, validId<DocumentId>("document.after-stop"))
            && !fixture.kernel.documentRuntime().open(fixture.document)
            && !fixture.kernel.documentRuntime().close(fixture.document)
            && !fixture.kernel.documentRuntime().detach(fixture.document)
            && !fixture.kernel.documentRuntime().remove(fixture.document)
            && !fixture.kernel.projectRuntime().open(fixture.project)
            && !fixture.kernel.projectRuntime().close(fixture.project);
    }
    release.set_value();
    auto stopped = stopping.get();
    REQUIRE(reached);
    CHECK(secondDenied);
    CHECK(requestsDenied);
    REQUIRE(reentered.has_value());
    CHECK_FALSE(reentered->hasValue());
    if(!*reentered) { CHECK(std::string(reentered->error().code.value()) == "Kernel.LifecycleInProgress"); }
    CHECK(handler->calls == 0U);
    CHECK(stops == 1U);
    REQUIRE(stopped);
    CHECK(fixture.kernel.state() == AppKernelState::Stopped);
    REQUIRE(fixture.kernel.shutdown());
}

TEST_CASE("DocumentRuntime blocks close while a document query is active",
          "[runtime][document][query][concurrency]")
{
    RuntimeFixture fixture;
    const auto sibling = validId<DocumentId>("document.project-sibling");
    REQUIRE(fixture.kernel.addDocument(fixture.project, sibling).hasValue());
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::promise<void> release;
    auto handler = std::make_shared<BlockingQueryHandler>(
        entered, release.get_future().share());
    REQUIRE(lasercnc::test::registerQuery(fixture.kernel,
        queryDescriptor("kernel.blocking-document-query", ExecutionScope::Document),
        handler).hasValue());
    REQUIRE(fixture.kernel.bootstrap().hasValue());

    auto running = std::async(std::launch::async, [&]() {
        return fixture.kernel.execution().executeQuery(QueryRequest {
            validId<RequestId>("request.blocking-document-query"),
            ExecutionContext {fixture.session, fixture.project, fixture.document},
            validId<QueryName>("kernel.blocking-document-query"),
            Version {1U, 0U, 0U},
            Value {Value::Object {}},
            validId<CorrelationId>("correlation.blocking-document-query"),
            validId<TraceId>("trace.blocking-document-query")});
    });
    enteredFuture.wait();

    auto projectClose = fixture.kernel.projectRuntime().close(fixture.project);
    CHECK_FALSE(projectClose.hasValue());
    CHECK(fixture.kernel.projectRuntime().lifecycle(fixture.project).value().state == ProjectLifecycleState::Open);
    CHECK(fixture.kernel.documents().contains(sibling));

    auto refused = fixture.kernel.documentRuntime().close(fixture.document);
    REQUIRE_FALSE(refused.hasValue());
    CHECK(std::string(refused.error().code.value()) == "Document.ActiveOperations");
    auto lifecycle = fixture.kernel.documentRuntime().lifecycle(fixture.document);
    REQUIRE(lifecycle.hasValue());
    CHECK(lifecycle.value().state == DocumentLifecycleState::Open);

    release.set_value();
    REQUIRE(running.get().hasValue());
    REQUIRE(fixture.kernel.documentRuntime().close(fixture.document).hasValue());
    REQUIRE(fixture.kernel.projectRuntime().close(fixture.project).hasValue());
    CHECK_FALSE(fixture.kernel.documents().contains(sibling));
    REQUIRE(fixture.kernel.shutdown().hasValue());
}
