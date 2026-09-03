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

CommandDescriptor commandDescriptor(
    const char* name,
    Version version = Version {1U, 0U, 0U},
    ContractStatus status = ContractStatus::Active)
{
    return CommandDescriptor {
        validId<CommandName>(name),
        version,
        schema("schema.command.arguments", SchemaKind::Object),
        schema("schema.command.result", SchemaKind::Object),
        ExecutionMode::Synchronous,
        SideEffectLevel::DocumentWrite,
        validId<CapabilityId>("document.write"),
        false,
        true,
        true,
        status};
}

QueryDescriptor queryDescriptor(
    const char* name,
    Version version = Version {1U, 0U, 0U},
    ContractStatus status = ContractStatus::Active)
{
    return QueryDescriptor {
        validId<QueryName>(name),
        version,
        schema("schema.query.arguments", SchemaKind::Object),
        schema("schema.query.result", SchemaKind::Object),
        validId<CapabilityId>("document.read"),
        ExecutionScope::Document,
        true,
        status};
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

TEST_CASE("ExecutionContext matches only its declared scope shape", "[runtime][execution][scope]")
{
    const auto session = validId<SessionId>("session.scope");
    const auto project = validId<ProjectId>("project.scope");
    const auto document = validId<DocumentId>("document.scope");
    const ExecutionContext sessionOnly {session, std::nullopt, std::nullopt};
    const ExecutionContext projectOnly {session, project, std::nullopt};
    const ExecutionContext documentContext {session, project, document};
    const ExecutionContext danglingDocument {session, std::nullopt, document};

    CHECK(contextMatchesScope(sessionOnly, ExecutionScope::Global));
    CHECK(contextMatchesScope(sessionOnly, ExecutionScope::Session));
    CHECK(contextMatchesScope(projectOnly, ExecutionScope::Project));
    CHECK(contextMatchesScope(documentContext, ExecutionScope::Document));
    CHECK_FALSE(contextMatchesScope(projectOnly, ExecutionScope::Session));
    CHECK_FALSE(contextMatchesScope(documentContext, ExecutionScope::Project));
    CHECK_FALSE(contextMatchesScope(danglingDocument, ExecutionScope::Document));
    CHECK_FALSE(contextMatchesScope(
        sessionOnly, static_cast<ExecutionScope>(255U)));
    CHECK(std::string(executionScopeName(ExecutionScope::Document)) == "document");
    CHECK(std::string(executionScopeName(static_cast<ExecutionScope>(255U))) == "unknown");
}

TEST_CASE("CommandRegistry resolves exact compatible and deprecated versions", "[runtime][command]")
{
    CommandRegistry registry;
    auto handler = std::make_shared<CommandHandler>();
    REQUIRE(registry.registerHandler(commandDescriptor("kernel.command.zulu"), handler).hasValue());
    REQUIRE(registry.registerHandler(commandDescriptor("kernel.command.alpha"), handler).hasValue());
    REQUIRE(registry.registerHandler(commandDescriptor(
        "kernel.command.alpha",
        Version {1U, 2U, 0U},
        ContractStatus::Deprecated), handler).hasValue());
    REQUIRE(registry.registerHandler(commandDescriptor(
        "kernel.command.alpha", Version {2U, 0U, 0U}), handler).hasValue());

    const auto descriptors = registry.descriptors();
    REQUIRE(descriptors.size() == 4U);
    CHECK(descriptors[0].name == validId<CommandName>("kernel.command.alpha"));
    CHECK(descriptors[0].version == Version {1U, 0U, 0U});
    CHECK(descriptors[1].version == Version {1U, 2U, 0U});
    CHECK(descriptors[2].version == Version {2U, 0U, 0U});
    CHECK(descriptors[3].name == validId<CommandName>("kernel.command.zulu"));

    const CommandKey alphaOne {
        validId<CommandName>("kernel.command.alpha"), Version {1U, 0U, 0U}};
    auto exact = registry.descriptor(alphaOne);
    REQUIRE(exact.hasValue());
    CHECK(exact.value().version == Version {1U, 0U, 0U});
    CHECK(exact.value().status == ContractStatus::Active);
    auto compatible = registry.descriptor(alphaOne, VersionResolution::Compatible);
    REQUIRE(compatible.hasValue());
    CHECK(compatible.value().version == Version {1U, 2U, 0U});
    CHECK(compatible.value().status == ContractStatus::Deprecated);

    auto unsupported = registry.descriptor(CommandKey {
        validId<CommandName>("kernel.command.alpha"), Version {1U, 3U, 0U}},
        VersionResolution::Compatible);
    REQUIRE_FALSE(unsupported.hasValue());
    CHECK(std::string(unsupported.error().code.value()) == "Command.UnsupportedVersion");

    auto duplicate = registry.registerHandler(commandDescriptor("kernel.command.alpha"), handler);
    REQUIRE_FALSE(duplicate.hasValue());
    CHECK(std::string(duplicate.error().code.value()) == "Command.AlreadyRegistered");

    auto asynchronous = commandDescriptor("kernel.command.async");
    asynchronous.executionMode = ExecutionMode::Asynchronous;
    auto asyncResult = registry.registerHandler(std::move(asynchronous), handler);
    REQUIRE_FALSE(asyncResult.hasValue());
    CHECK(std::string(asyncResult.error().code.value()) == "Command.HandlerModeMismatch");

    auto unsafeSideEffect = commandDescriptor("kernel.command.file");
    unsafeSideEffect.sideEffect = SideEffectLevel::FileSystemWrite;
    CHECK_FALSE(registry.registerHandler(std::move(unsafeSideEffect), handler).hasValue());

    auto invalidScope = commandDescriptor("kernel.command.project-write");
    invalidScope.scope = ExecutionScope::Project;
    auto invalidScopeResult = registry.registerHandler(std::move(invalidScope), handler);
    REQUIRE_FALSE(invalidScopeResult.hasValue());
    CHECK(std::string(invalidScopeResult.error().code.value())
          == "Command.ScopeSideEffectMismatch");

    auto undoable = commandDescriptor("kernel.command.undoable");
    undoable.undoable = true;
    CHECK_FALSE(registry.registerHandler(std::move(undoable), handler).hasValue());
    CHECK_FALSE(registry.registerHandler(
        commandDescriptor("kernel.command.null"), nullptr).hasValue());
    auto missing = registry.descriptor(CommandKey {
        validId<CommandName>("kernel.command.missing"), Version {1U, 0U, 0U}});
    REQUIRE_FALSE(missing.hasValue());
    CHECK(std::string(missing.error().code.value()) == "Command.NotFound");
}

TEST_CASE("QueryRegistry resolves exact compatible and deprecated versions", "[runtime][query]")
{
    QueryRegistry registry;
    auto handler = std::make_shared<QueryHandler>();
    REQUIRE(registry.registerHandler(queryDescriptor("kernel.query.status"), handler).hasValue());
    REQUIRE(registry.registerHandler(queryDescriptor(
        "kernel.query.status",
        Version {1U, 1U, 0U},
        ContractStatus::Deprecated), handler).hasValue());
    CHECK(registry.size() == 2U);
    const QueryKey statusOne {
        validId<QueryName>("kernel.query.status"), Version {1U, 0U, 0U}};
    auto exact = registry.descriptor(statusOne);
    REQUIRE(exact.hasValue());
    CHECK(exact.value().version == Version {1U, 0U, 0U});
    auto compatible = registry.descriptor(statusOne, VersionResolution::Compatible);
    REQUIRE(compatible.hasValue());
    CHECK(compatible.value().version == Version {1U, 1U, 0U});
    CHECK(compatible.value().status == ContractStatus::Deprecated);

    auto duplicate = registry.registerHandler(queryDescriptor("kernel.query.status"), handler);
    REQUIRE_FALSE(duplicate.hasValue());
    CHECK(std::string(duplicate.error().code.value()) == "Query.AlreadyRegistered");
    CHECK_FALSE(registry.registerHandler(
        queryDescriptor("kernel.query.null"), nullptr).hasValue());
    auto invalidScope = queryDescriptor("kernel.query.invalid-scope");
    invalidScope.scope = static_cast<ExecutionScope>(255U);
    auto invalidScopeResult = registry.registerHandler(std::move(invalidScope), handler);
    REQUIRE_FALSE(invalidScopeResult.hasValue());
    CHECK(std::string(invalidScopeResult.error().code.value()) == "Query.InvalidScope");
    auto unsupported = registry.descriptor(QueryKey {
        validId<QueryName>("kernel.query.status"), Version {2U, 0U, 0U}},
        VersionResolution::Compatible);
    REQUIRE_FALSE(unsupported.hasValue());
    CHECK(std::string(unsupported.error().code.value()) == "Query.UnsupportedVersion");
    auto missing = registry.descriptor(QueryKey {
        validId<QueryName>("kernel.query.missing"), Version {1U, 0U, 0U}});
    REQUIRE_FALSE(missing.hasValue());
    CHECK(std::string(missing.error().code.value()) == "Query.NotFound");
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
