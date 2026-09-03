#include <lasercnc/kernel/app_kernel.hpp>
#include <lasercnc/observability/log_service.hpp>
#include <lasercnc/observability/metrics_service.hpp>
#include <lasercnc/observability/trace_service.hpp>
#include <lasercnc/runtime/command_runtime.hpp>
#include <lasercnc/runtime/query_runtime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
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

QueryDescriptor queryDescriptor(const char* name, bool requiresDocument = true)
{
    return QueryDescriptor {
        validId<QueryName>(name),
        Version {1U, 0U, 0U},
        schema("schema.query.arguments.object", SchemaKind::Object),
        schema("schema.query.result.object", SchemaKind::Object),
        validId<CapabilityId>("document.read"),
        requiresDocument,
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
        session,
        project,
        document,
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
        session,
        project,
        document,
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
        REQUIRE(kernel.addDocument(project, document).hasValue());
        REQUIRE(kernel.executionServices().configure(validator, log).hasValue());
        const std::array grants {
            validId<CapabilityId>("document.read"),
            validId<CapabilityId>("document.write")};
        REQUIRE(kernel.capabilities().replace(session, grants).hasValue());
    }

    void registerStandardHandlers()
    {
        create = std::make_shared<CreateObjectHandler>();
        query = std::make_shared<ObjectQueryHandler>();
        REQUIRE(kernel.commandRegistry().registerHandler(
            commandDescriptor("kernel.object.create"), create).hasValue());
        REQUIRE(kernel.queryRegistry().registerHandler(
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

    auto beforeReady = fixture.kernel.commands().execute(commandRequest(
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
    CHECK(fixture.kernel.commands().accepting());
    CHECK(fixture.kernel.queries().accepting());
    CHECK_FALSE(fixture.kernel.commandRegistry().registerHandler(
        commandDescriptor("kernel.late"), fixture.create).hasValue());

    auto createRequest = commandRequest(
        "request.create", fixture.project, fixture.document, fixture.session,
        "kernel.object.create", "object.runtime");
    createRequest.parentSpanId = validId<SpanId>("span.workflow.command-parent");
    auto command = fixture.kernel.commands().execute(createRequest);
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
    auto query = fixture.kernel.queries().execute(getRequest);
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
    CHECK_FALSE(fixture.kernel.commands().accepting());
    CHECK_FALSE(fixture.kernel.queries().execute(queryRequest(
        fixture.project, fixture.document, fixture.session, "object.runtime")).hasValue());
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
    REQUIRE(fixture.kernel.commandRegistry().registerHandler(
        commandOne, fixture.create).hasValue());
    REQUIRE(fixture.kernel.commandRegistry().registerHandler(
        commandOneTwo, fixture.create).hasValue());

    auto queryOne = queryDescriptor("kernel.versioned.get");
    auto queryOneOne = queryOne;
    queryOneOne.version = Version {1U, 1U, 0U};
    queryOneOne.status = ContractStatus::Deprecated;
    REQUIRE(fixture.kernel.queryRegistry().registerHandler(
        queryOne, fixture.query).hasValue());
    REQUIRE(fixture.kernel.queryRegistry().registerHandler(
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
    auto unsupportedResult = fixture.kernel.commands().execute(unsupported);
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
    auto command = fixture.kernel.commands().execute(compatible);
    REQUIRE(command.hasValue());
    CHECK(command.value().resolvedVersion == Version {1U, 2U, 0U});
    CHECK(command.value().contractStatus == ContractStatus::Deprecated);

    auto queryRequestValue = queryRequest(
        fixture.project, fixture.document, fixture.session, "object.versioned");
    queryRequestValue.query = validId<QueryName>("kernel.versioned.get");
    queryRequestValue.versionResolution = VersionResolution::Compatible;
    auto query = fixture.kernel.queries().execute(queryRequestValue);
    REQUIRE(query.hasValue());
    CHECK(query.value().resolvedVersion == Version {1U, 1U, 0U});
    CHECK(query.value().contractStatus == ContractStatus::Deprecated);
    CHECK(fixture.create->calls == 1U);
    CHECK(fixture.query->calls == 1U);

    REQUIRE(fixture.kernel.shutdown().hasValue());
}

TEST_CASE("CommandRuntime enforces schema capability project and revision before writes", "[runtime][command]")
{
    RuntimeFixture fixture;
    fixture.registerStandardHandlers();
    REQUIRE(fixture.kernel.bootstrap().hasValue());

    auto invalidSchema = commandRequest(
        "request.invalid-schema", fixture.project, fixture.document, fixture.session,
        "kernel.object.create", "object.invalid-schema");
    invalidSchema.arguments = Value {"not-an-object"};
    CHECK_FALSE(fixture.kernel.commands().execute(invalidSchema).hasValue());
    CHECK(fixture.create->calls == 0U);

    const std::array<CapabilityId, 0U> noCapabilities {};
    REQUIRE(fixture.kernel.capabilities().replace(fixture.session, noCapabilities).hasValue());
    auto denied = fixture.kernel.commands().execute(commandRequest(
        "request.denied", fixture.project, fixture.document, fixture.session,
        "kernel.object.create", "object.denied"));
    REQUIRE_FALSE(denied.hasValue());
    CHECK(std::string(denied.error().code.value()) == "Capability.Denied");
    CHECK(fixture.create->calls == 0U);

    const std::array grants {validId<CapabilityId>("document.write")};
    REQUIRE(fixture.kernel.capabilities().replace(fixture.session, grants).hasValue());
    auto wrongProject = fixture.kernel.commands().execute(commandRequest(
        "request.wrong-project", validId<ProjectId>("project.other"), fixture.document,
        fixture.session, "kernel.object.create", "object.wrong-project"));
    REQUIRE_FALSE(wrongProject.hasValue());
    CHECK(std::string(wrongProject.error().code.value()) == "Command.ProjectMismatch");

    auto first = fixture.kernel.commands().execute(commandRequest(
        "request.first", fixture.project, fixture.document, fixture.session,
        "kernel.object.create", "object.first"));
    REQUIRE(first.hasValue());
    auto stale = fixture.kernel.commands().execute(commandRequest(
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
    REQUIRE(fixture.kernel.commandRegistry().registerHandler(
        commandDescriptor("kernel.fail"), failing).hasValue());
    REQUIRE(fixture.kernel.commandRegistry().registerHandler(
        commandDescriptor("kernel.throw"), throwing).hasValue());
    auto badResult = std::make_shared<CreateObjectHandler>();
    auto badDescriptor = commandDescriptor("kernel.bad-result");
    badDescriptor.result = schema("schema.command.result.string", SchemaKind::String);
    REQUIRE(fixture.kernel.commandRegistry().registerHandler(
        std::move(badDescriptor), badResult).hasValue());
    REQUIRE(fixture.kernel.bootstrap().hasValue());

    auto failed = fixture.kernel.commands().execute(commandRequest(
        "request.fail", fixture.project, fixture.document, fixture.session,
        "kernel.fail", "object.ignored"));
    REQUIRE_FALSE(failed.hasValue());
    CHECK(std::string(failed.error().code.value()) == "Test.HandlerRejected");

    auto threw = fixture.kernel.commands().execute(commandRequest(
        "request.throw", fixture.project, fixture.document, fixture.session,
        "kernel.throw", "object.ignored"));
    REQUIRE_FALSE(threw.hasValue());
    CHECK(std::string(threw.error().code.value()) == "Command.HandlerFailed");

    auto invalidResult = fixture.kernel.commands().execute(commandRequest(
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
    auto first = fixture.kernel.commands().execute(commandRequest(
        "request.idempotency.first", fixture.project, fixture.document, fixture.session,
        "kernel.object.create", "object.idempotent", key));
    REQUIRE(first.hasValue());
    CHECK_FALSE(first.value().replayed);

    auto replay = fixture.kernel.commands().execute(commandRequest(
        "request.idempotency.retry", fixture.project, fixture.document, fixture.session,
        "kernel.object.create", "object.idempotent", key));
    REQUIRE(replay.hasValue());
    CHECK(replay.value().replayed);
    REQUIRE(replay.value().commit.has_value());
    REQUIRE(first.value().commit.has_value());
    CHECK(replay.value().commit->transactionId == first.value().commit->transactionId);
    CHECK(fixture.create->calls == 1U);
    CHECK(eventCount == 1U);
    CHECK(fixture.kernel.commands().idempotencyRecordCount() == 1U);

    auto resolutionChanged = commandRequest(
        "request.idempotency.resolution-conflict",
        fixture.project,
        fixture.document,
        fixture.session,
        "kernel.object.create",
        "object.idempotent",
        key);
    resolutionChanged.versionResolution = VersionResolution::Compatible;
    auto resolutionConflict = fixture.kernel.commands().execute(resolutionChanged);
    REQUIRE_FALSE(resolutionConflict.hasValue());
    CHECK(std::string(resolutionConflict.error().code.value())
          == "Command.IdempotencyKeyConflict");
    CHECK(fixture.create->calls == 1U);

    auto rebound = fixture.kernel.commands().execute(commandRequest(
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
    REQUIRE(fixture.kernel.commandRegistry().registerHandler(
        commandDescriptor("kernel.slow-create"), handler).hasValue());
    REQUIRE(fixture.kernel.bootstrap().hasValue());
    const auto key = validId<IdempotencyKey>("idempotency.concurrent");

    auto first = std::async(std::launch::async, [&]() {
        return fixture.kernel.commands().execute(commandRequest(
            "request.concurrent.first", fixture.project, fixture.document, fixture.session,
            "kernel.slow-create", "unused", key));
    });
    enteredFuture.wait();
    auto refused = fixture.kernel.shutdown();
    REQUIRE_FALSE(refused.hasValue());
    CHECK(std::string(refused.error().code.value()) == "Kernel.ActiveTransactions");
    CHECK(fixture.kernel.state() == lasercnc::kernel::AppKernelState::Ready);
    auto second = std::async(std::launch::async, [&]() {
        return fixture.kernel.commands().execute(commandRequest(
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

    auto command = fixture.kernel.commands().execute(commandRequest(
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
    missingDocument.documentId.reset();
    auto missing = fixture.kernel.queries().execute(missingDocument);
    REQUIRE_FALSE(missing.hasValue());
    CHECK(std::string(missing.error().code.value()) == "Query.DocumentRequired");

    const std::array<CapabilityId, 0U> noCapabilities {};
    REQUIRE(fixture.kernel.capabilities().replace(fixture.session, noCapabilities).hasValue());
    auto denied = fixture.kernel.queries().execute(queryRequest(
        fixture.project, fixture.document, fixture.session, "object.missing"));
    REQUIRE_FALSE(denied.hasValue());
    CHECK(std::string(denied.error().code.value()) == "Capability.Denied");
    CHECK(fixture.query->calls == 0U);
}

TEST_CASE("AppKernel refuses registered runtimes without execution services", "[kernel][runtime]")
{
    lasercnc::kernel::AppKernel kernel;
    REQUIRE(kernel.commandRegistry().registerHandler(
        commandDescriptor("kernel.unconfigured"),
        std::make_shared<CreateObjectHandler>()).hasValue());
    auto bootstrapped = kernel.bootstrap();
    REQUIRE_FALSE(bootstrapped.hasValue());
    CHECK(std::string(bootstrapped.error().code.value())
          == "Runtime.ExecutionServicesNotConfigured");
    CHECK(kernel.state() == lasercnc::kernel::AppKernelState::Failed);
    CHECK_FALSE(kernel.commands().accepting());
}

TEST_CASE("AppKernel refuses shutdown while a query execution is active", "[kernel][runtime][query]")
{
    RuntimeFixture fixture;
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::promise<void> release;
    auto handler = std::make_shared<BlockingQueryHandler>(
        entered, release.get_future().share());
    REQUIRE(fixture.kernel.queryRegistry().registerHandler(
        queryDescriptor("kernel.blocking-query", false), handler).hasValue());
    REQUIRE(fixture.kernel.bootstrap().hasValue());

    auto running = std::async(std::launch::async, [&]() {
        return fixture.kernel.queries().execute(QueryRequest {
            validId<RequestId>("request.blocking-query"),
            fixture.session,
            fixture.project,
            std::nullopt,
            validId<QueryName>("kernel.blocking-query"),
            Version {1U, 0U, 0U},
            Value {Value::Object {}},
            validId<CorrelationId>("correlation.blocking-query"),
            validId<TraceId>("trace.blocking-query")});
    });
    enteredFuture.wait();
    CHECK(fixture.kernel.queries().activeExecutionCount() == 1U);
    auto refused = fixture.kernel.shutdown();
    REQUIRE_FALSE(refused.hasValue());
    CHECK(std::string(refused.error().code.value()) == "Kernel.ActiveExecutions");
    CHECK(fixture.kernel.state() == lasercnc::kernel::AppKernelState::Ready);

    release.set_value();
    REQUIRE(running.get().hasValue());
    REQUIRE(fixture.kernel.shutdown().hasValue());
}
