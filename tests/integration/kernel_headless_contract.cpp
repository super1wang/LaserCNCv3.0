#include <lasercnc/infrastructure/jsoncons_adapter.hpp>
#include <lasercnc/infrastructure/spdlog_log_service.hpp>
#include <lasercnc/kernel/app_kernel.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

using namespace lasercnc::foundation;
using namespace lasercnc::infrastructure;
using namespace lasercnc::kernel;
using namespace lasercnc::messaging;
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
    auto registeredCommand = kernel.commandRegistry().registerHandler(
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
    auto registeredQuery = kernel.queryRegistry().registerHandler(
        QueryDescriptor {
            requiredId<QueryName>("kernel.contract.object.get"),
            Version {1U, 0U, 0U},
            std::move(getArguments).value(),
            std::move(getResult).value(),
            requiredId<CapabilityId>("document.read"),
            true,
            true},
        std::make_shared<GetObjectHandler>());
    if(!registeredQuery.hasValue()) {
        return fail("query registration", registeredQuery.error());
    }
    const auto commandDescriptors = kernel.commandRegistry().descriptors();
    const auto queryDescriptors = kernel.queryRegistry().descriptors();
    if(commandDescriptors.size() != 1U || queryDescriptors.size() != 1U
       || commandDescriptors.front().name
           != requiredId<CommandName>("kernel.contract.object.put")
       || queryDescriptors.front().name
           != requiredId<QueryName>("kernel.contract.object.get")) {
        std::cerr << "discovery: deterministic descriptors are missing\n";
        return 1;
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

    auto parsedArguments = validator->deserialize(R"({"id":"object.cli","data":"verified"})");
    if(!parsedArguments.hasValue()) {
        return fail("argument parse", parsedArguments.error());
    }
    auto command = kernel.commands().execute(CommandRequest {
        requiredId<RequestId>("request.cli.command"),
        session,
        project,
        document,
        requiredId<CommandName>("kernel.contract.object.put"),
        std::move(parsedArguments).value(),
        Revision {0U},
        requiredId<CorrelationId>("correlation.cli"),
        requiredId<TraceId>("trace.cli"),
        requiredId<IdempotencyKey>("idempotency.cli.put")});
    if(!command.hasValue()) {
        return fail("command execute", command.error());
    }
    if(eventCount != 1U || command.value().replayed
       || !command.value().postCommitErrors.empty()) {
        std::cerr << "command execute: invalid commit or event result\n";
        return 1;
    }

    auto queryArguments = validator->deserialize(R"({"id":"object.cli"})");
    if(!queryArguments.hasValue()) {
        return fail("query parse", queryArguments.error());
    }
    auto query = kernel.queries().execute(QueryRequest {
        requiredId<RequestId>("request.cli.query"),
        session,
        project,
        document,
        requiredId<QueryName>("kernel.contract.object.get"),
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
    if(!std::filesystem::exists(logPath) || std::filesystem::file_size(logPath) == 0U) {
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

} // namespace

int main(int argc, char** argv)
{
    if(argc != 3 || std::string(argv[1]) != "--mode"
       || std::string(argv[2]) != "roundtrip") {
        std::cerr << "usage: lasercnc_kernel_headless_contract --mode roundtrip\n";
        return 2;
    }
    try {
        return runRoundTrip();
    } catch(const std::exception& exception) {
        std::cerr << "unexpected: " << exception.what() << '\n';
        return 3;
    } catch(...) {
        std::cerr << "unexpected: unknown failure\n";
        return 3;
    }
}
