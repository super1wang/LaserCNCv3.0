#include <lasercnc/runtime/capability_service.hpp>
#include <lasercnc/runtime/command_registry.hpp>
#include <lasercnc/runtime/execution_services.hpp>
#include <lasercnc/runtime/query_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <stdexcept>
#include <string>

using namespace lasercnc::foundation;
using namespace lasercnc::kernel;
using namespace lasercnc::runtime;

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
    auto created = Schema::create(
        validId<SchemaId>(id), Version {1U, 0U, 0U}, kind);
    if(!created.hasValue()) {
        throw std::logic_error("Invalid test schema");
    }
    return std::move(created).value();
}

CommandDescriptor commandDescriptor(const char* name)
{
    return CommandDescriptor {
        validId<CommandName>(name),
        Version {1U, 0U, 0U},
        schema("schema.command.arguments", SchemaKind::Object),
        schema("schema.command.result", SchemaKind::Object),
        ExecutionMode::Synchronous,
        SideEffectLevel::DocumentWrite,
        validId<CapabilityId>("document.write"),
        false,
        true,
        true};
}

QueryDescriptor queryDescriptor(const char* name)
{
    return QueryDescriptor {
        validId<QueryName>(name),
        Version {1U, 0U, 0U},
        schema("schema.query.arguments", SchemaKind::Object),
        schema("schema.query.result", SchemaKind::Object),
        validId<CapabilityId>("document.read"),
        true,
        true};
}

class CommandHandler final : public ICommandHandler {
public:
    Result<Value> execute(const CommandRequest&, ApplicationTransaction&) override
    {
        return Result<Value>::success(Value {});
    }
};

class QueryHandler final : public IQueryHandler {
public:
    Result<Value> execute(const QueryRequest&, const QueryContext&) override
    {
        return Result<Value>::success(Value {});
    }
};

} // namespace

TEST_CASE("CapabilityService is deny by default and replaces exact grants", "[runtime][capability]")
{
    CapabilityService capabilities;
    const auto session = validId<SessionId>("session.operator");
    const auto read = validId<CapabilityId>("document.read");
    const auto write = validId<CapabilityId>("document.write");

    auto denied = capabilities.authorize(session, read);
    REQUIRE_FALSE(denied.hasValue());
    CHECK(std::string(denied.error().code.value()) == "Capability.Denied");

    const std::array grants {write, read, read};
    REQUIRE(capabilities.replace(session, grants).hasValue());
    CHECK(capabilities.authorize(session, read).hasValue());
    CHECK(capabilities.authorize(session, write).hasValue());
    const auto snapshot = capabilities.snapshot(session);
    REQUIRE(snapshot.capabilities.size() == 2U);
    CHECK(snapshot.capabilities[0] == read);
    CHECK(snapshot.capabilities[1] == write);

    const std::array readOnly {read};
    REQUIRE(capabilities.replace(session, readOnly).hasValue());
    CHECK_FALSE(capabilities.authorize(session, write).hasValue());
    REQUIRE(capabilities.remove(session).hasValue());
    CHECK_FALSE(capabilities.authorize(session, read).hasValue());
    CHECK_FALSE(capabilities.remove(session).hasValue());
}

TEST_CASE("CommandRegistry validates Phase 5 boundaries and discovers deterministically", "[runtime][command]")
{
    CommandRegistry registry;
    auto handler = std::make_shared<CommandHandler>();
    REQUIRE(registry.registerHandler(commandDescriptor("kernel.command.zulu"), handler).hasValue());
    REQUIRE(registry.registerHandler(commandDescriptor("kernel.command.alpha"), handler).hasValue());

    const auto descriptors = registry.descriptors();
    REQUIRE(descriptors.size() == 2U);
    CHECK(descriptors[0].name == validId<CommandName>("kernel.command.alpha"));
    CHECK(descriptors[1].name == validId<CommandName>("kernel.command.zulu"));
    CHECK(registry.descriptor(descriptors[0].name).hasValue());

    auto duplicate = registry.registerHandler(commandDescriptor("kernel.command.alpha"), handler);
    REQUIRE_FALSE(duplicate.hasValue());
    CHECK(std::string(duplicate.error().code.value()) == "Command.AlreadyRegistered");

    auto asynchronous = commandDescriptor("kernel.command.async");
    asynchronous.executionMode = ExecutionMode::Asynchronous;
    auto asyncResult = registry.registerHandler(std::move(asynchronous), handler);
    REQUIRE_FALSE(asyncResult.hasValue());
    CHECK(std::string(asyncResult.error().code.value()) == "Command.ExecutionModeUnsupported");

    auto unsafeSideEffect = commandDescriptor("kernel.command.file");
    unsafeSideEffect.sideEffect = SideEffectLevel::FileSystemWrite;
    CHECK_FALSE(registry.registerHandler(std::move(unsafeSideEffect), handler).hasValue());

    auto undoable = commandDescriptor("kernel.command.undoable");
    undoable.undoable = true;
    CHECK_FALSE(registry.registerHandler(std::move(undoable), handler).hasValue());
    CHECK_FALSE(registry.registerHandler(
        commandDescriptor("kernel.command.null"), nullptr).hasValue());
    CHECK_FALSE(registry.descriptor(validId<CommandName>("kernel.command.missing")).hasValue());
}

TEST_CASE("QueryRegistry rejects ambiguity and exposes descriptor snapshots", "[runtime][query]")
{
    QueryRegistry registry;
    auto handler = std::make_shared<QueryHandler>();
    REQUIRE(registry.registerHandler(queryDescriptor("kernel.query.status"), handler).hasValue());
    CHECK(registry.size() == 1U);
    REQUIRE(registry.descriptor(validId<QueryName>("kernel.query.status")).hasValue());

    auto duplicate = registry.registerHandler(queryDescriptor("kernel.query.status"), handler);
    REQUIRE_FALSE(duplicate.hasValue());
    CHECK(std::string(duplicate.error().code.value()) == "Query.AlreadyRegistered");
    CHECK_FALSE(registry.registerHandler(
        queryDescriptor("kernel.query.null"), nullptr).hasValue());
    CHECK_FALSE(registry.descriptor(validId<QueryName>("kernel.query.missing")).hasValue());
}

TEST_CASE("ExecutionServices requires both replaceable Kernel ports", "[runtime][execution]")
{
    ExecutionServices services;
    CHECK_FALSE(services.configured());
    auto missing = services.snapshot();
    REQUIRE_FALSE(missing.hasValue());
    CHECK(std::string(missing.error().code.value()) == "Runtime.ExecutionServicesNotConfigured");
    CHECK_FALSE(services.configure(nullptr, nullptr).hasValue());
}
