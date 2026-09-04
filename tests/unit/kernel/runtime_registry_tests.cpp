#include <lasercnc/runtime/capability_service.hpp>
#include <lasercnc/runtime/command_registry.hpp>
#include <lasercnc/runtime/execution_services.hpp>
#include <lasercnc/runtime/effect_guard.hpp>
#include <lasercnc/runtime/query_registry.hpp>
#include <lasercnc/foundation/error.hpp>

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

class AsyncHandler final : public IAsyncCommandHandler {
public:
    Result<AsyncCommandPlan> prepare(const CommandRequest&) override
    {
        return Result<AsyncCommandPlan>::failure(makeError("Test.UnexpectedExecution",
            ErrorCategory::Internal, "Registration must not prepare a task"));
    }
};

class ReadOnlyHandler final : public IReadOnlyCommandHandler {
public:
    Result<Value> execute(const CommandRequest&, const ReadOnlyCommandContext&) override
    {
        return Result<Value>::success(Value {});
    }
};

class ExternalEffectHandler final : public IExternalEffectHandler {
public:
    Result<Value> execute(const CommandRequest&, const ExternalEffectContext&) override
    {
        return Result<Value>::success(Value {});
    }
};

class AllowEffectGuard final : public IEffectGuard {
public:
    Result<void> evaluate(
        const CommandRequest&,
        const CommandDescriptor&,
        const EffectGuardContext&) override
    {
        return Result<void>::success();
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
    CHECK(registry.registerHandler(std::move(undoable), handler).hasValue());
    CHECK_FALSE(registry.registerHandler(
        commandDescriptor("kernel.command.null"), nullptr).hasValue());
    auto missing = registry.descriptor(CommandKey {
        validId<CommandName>("kernel.command.missing"), Version {1U, 0U, 0U}});
    REQUIRE_FALSE(missing.hasValue());
    CHECK(std::string(missing.error().code.value()) == "Command.NotFound");
}

TEST_CASE("Execution registries reject unknown contract status across every handler family", "[runtime][contract][status]")
{
    for(const unsigned int raw : {0U, 1U, 2U, 127U, 255U}) {
        for(unsigned int family = 0U; family < 5U; ++family) {
            DYNAMIC_SECTION("status=" << raw << " family=" << family) {
                const bool known = raw <= 1U;
                const auto status = static_cast<ContractStatus>(raw);
                if(family == 4U) {
                    QueryRegistry registry;
                    const auto registered = registry.registerHandler(
                        queryDescriptor("query.status", {1U, 0U, 0U}, status), std::make_shared<QueryHandler>());
                    CHECK(registered.hasValue() == known);
                    CHECK(registry.size() == (known ? 1U : 0U));
                    if(!registered) { CHECK(std::string(registered.error().code.value()) == "Query.InvalidStatus"); }
                } else {
                    CommandRegistry registry;
                    auto descriptor = commandDescriptor("command.status", {1U, 0U, 0U}, status);
                    const auto registered = [&]() {
                        if(family == 0U) { return registry.registerHandler(descriptor, std::make_shared<CommandHandler>()); }
                        descriptor.sideEffect = SideEffectLevel::ReadOnly;
                        if(family == 1U) {
                            descriptor.idempotent = false;
                            return registry.registerReadOnlyHandler(descriptor, std::make_shared<ReadOnlyHandler>());
                        }
                        if(family == 2U) {
                            descriptor.executionMode = ExecutionMode::Asynchronous;
                            return registry.registerAsyncHandler(descriptor, std::make_shared<AsyncHandler>());
                        }
                        descriptor.sideEffect = SideEffectLevel::Publish;
                        descriptor.effectGuards = {validId<EffectGuardId>("guard.status")};
                        descriptor.resources = {{ResourceKind::DiskIO, validId<ResourceId>("resource.status"),
                            ResourceAccess::Exclusive, 1U}};
                        return registry.registerExternalEffectHandler(descriptor, std::make_shared<ExternalEffectHandler>());
                    }();
                    CHECK(registered.hasValue() == known);
                    CHECK(registry.size() == (known ? 1U : 0U));
                    if(!registered) { CHECK(std::string(registered.error().code.value()) == "Command.InvalidStatus"); }
                }
            }
        }
    }
}

TEST_CASE("CommandRegistry keeps synchronous read-only handlers outside transactions", "[runtime][command][scope]")
{
    CommandRegistry registry;
    auto handler = std::make_shared<ReadOnlyHandler>();
    auto descriptor = commandDescriptor("kernel.command.read-only");
    descriptor.sideEffect = SideEffectLevel::ReadOnly;
    descriptor.capability = validId<CapabilityId>("system.read");
    descriptor.idempotent = false;
    descriptor.scope = ExecutionScope::Global;
    REQUIRE(registry.registerReadOnlyHandler(descriptor, handler).hasValue());

    auto transactional = descriptor;
    transactional.name = validId<CommandName>("kernel.command.read-only-writes");
    transactional.sideEffect = SideEffectLevel::DocumentWrite;
    auto wrongSideEffect = registry.registerReadOnlyHandler(transactional, handler);
    REQUIRE_FALSE(wrongSideEffect.hasValue());
    CHECK(std::string(wrongSideEffect.error().code.value())
          == "Command.ReadOnlySideEffectMismatch");

    auto idempotent = descriptor;
    idempotent.name = validId<CommandName>("kernel.command.read-only-idempotent");
    idempotent.idempotent = true;
    auto wrongIdempotency = registry.registerReadOnlyHandler(idempotent, handler);
    REQUIRE_FALSE(wrongIdempotency.hasValue());
    CHECK(std::string(wrongIdempotency.error().code.value())
          == "Command.ReadOnlyIdempotencyUnsupported");
    auto externalMetadata = descriptor;
    externalMetadata.name = validId<CommandName>("kernel.command.read-only-effect-metadata");
    externalMetadata.effectGuards = {validId<EffectGuardId>("guard.invalid")};
    auto metadataRejected = registry.registerReadOnlyHandler(externalMetadata, handler);
    REQUIRE_FALSE(metadataRejected.hasValue());
    CHECK(std::string(metadataRejected.error().code.value())
          == "Command.ExternalMetadataUnsupported");
    CHECK_FALSE(registry.registerReadOnlyHandler(descriptor, nullptr).hasValue());
}

TEST_CASE("External effect registration requires replay guard resource and stable identity", "[runtime][command][effect]")
{
    CommandRegistry registry;
    auto handler = std::make_shared<ExternalEffectHandler>();
    auto descriptor = commandDescriptor("kernel.command.external-effect");
    descriptor.sideEffect = SideEffectLevel::Publish;
    descriptor.scope = ExecutionScope::Global;
    descriptor.replayPolicy = ReplayPolicy::ReconcileOnly;
    descriptor.effectGuards = {validId<EffectGuardId>("guard.publish")};
    descriptor.resources = {ResourceClaim {
        ResourceKind::DiskIO,
        validId<ResourceId>("resource.publish"),
        ResourceAccess::Exclusive,
        1U}};
    REQUIRE(registry.registerExternalEffectHandler(descriptor, handler).hasValue());

    auto noIdentity = descriptor;
    noIdentity.name = validId<CommandName>("kernel.command.external-no-identity");
    noIdentity.idempotent = false;
    auto identityRejected = registry.registerExternalEffectHandler(noIdentity, handler);
    REQUIRE_FALSE(identityRejected.hasValue());
    CHECK(std::string(identityRejected.error().code.value())
          == "Command.ExternalIdempotencyRequired");

    auto noGuard = descriptor;
    noGuard.name = validId<CommandName>("kernel.command.external-no-guard");
    noGuard.effectGuards.clear();
    auto guardRejected = registry.registerExternalEffectHandler(noGuard, handler);
    REQUIRE_FALSE(guardRejected.hasValue());
    CHECK(std::string(guardRejected.error().code.value())
          == "Command.EffectGuardRequired");

    auto noResource = descriptor;
    noResource.name = validId<CommandName>("kernel.command.external-no-resource");
    noResource.resources.clear();
    auto resourceRejected = registry.registerExternalEffectHandler(noResource, handler);
    REQUIRE_FALSE(resourceRejected.hasValue());
    CHECK(std::string(resourceRejected.error().code.value())
          == "Command.EffectResourceRequired");

    auto undoable = descriptor;
    undoable.name = validId<CommandName>("kernel.command.external-undoable");
    undoable.undoable = true;
    CHECK_FALSE(registry.registerExternalEffectHandler(undoable, handler).hasValue());
    auto invalidSideEffect = descriptor;
    invalidSideEffect.name = validId<CommandName>("kernel.command.external-invalid-effect");
    invalidSideEffect.sideEffect = static_cast<SideEffectLevel>(255U);
    CHECK_FALSE(registry.registerExternalEffectHandler(invalidSideEffect, handler).hasValue());
    auto zeroUnits = descriptor;
    zeroUnits.name = validId<CommandName>("kernel.command.external-zero-resource");
    zeroUnits.resources.front().units = 0U;
    CHECK_FALSE(registry.registerExternalEffectHandler(zeroUnits, handler).hasValue());
    CHECK_FALSE(registry.registerExternalEffectHandler(descriptor, nullptr).hasValue());
}

TEST_CASE("External effect registration rejects unknown resource enum values",
          "[runtime][command][effect][resource][c6b14]")
{
    CommandRegistry registry;
    auto handler = std::make_shared<ExternalEffectHandler>();
    auto descriptor = commandDescriptor("kernel.command.external-invalid-resource-kind");
    descriptor.sideEffect = SideEffectLevel::Publish;
    descriptor.scope = ExecutionScope::Global;
    descriptor.replayPolicy = ReplayPolicy::ReconcileOnly;
    descriptor.effectGuards = {validId<EffectGuardId>("guard.invalid-resource")};
    descriptor.resources = {ResourceClaim {
        static_cast<ResourceKind>(255U),
        validId<ResourceId>("resource.invalid-enum"),
        ResourceAccess::Exclusive,
        1U}};
    auto invalidKind = registry.registerExternalEffectHandler(descriptor, handler);
    CHECK_FALSE(invalidKind.hasValue());
    if(!invalidKind) {
        CHECK(std::string(invalidKind.error().code.value())
              == "Command.InvalidEffectResourceKind");
    }

    descriptor.name = validId<CommandName>("kernel.command.external-invalid-resource-access");
    descriptor.resources.front().kind = ResourceKind::DiskIO;
    descriptor.resources.front().access = static_cast<ResourceAccess>(255U);
    auto invalidAccess = registry.registerExternalEffectHandler(descriptor, handler);
    CHECK_FALSE(invalidAccess.hasValue());
    if(!invalidAccess) {
        CHECK(std::string(invalidAccess.error().code.value())
              == "Command.InvalidEffectResourceAccess");
    }
    CHECK(registry.size() == 0U);
}

TEST_CASE("EffectGuardRegistry freezes stable guard identities", "[runtime][effect][guard]")
{
    EffectGuardRegistry guards;
    const auto id = validId<EffectGuardId>("guard.machine-ready");
    auto guard = std::make_shared<AllowEffectGuard>();
    REQUIRE(guards.registerGuard(id, guard).hasValue());
    REQUIRE(guards.guard(id).hasValue());
    CHECK(guards.ids() == std::vector<EffectGuardId> {id});
    CHECK_FALSE(guards.registerGuard(id, guard).hasValue());
    CHECK_FALSE(guards.registerGuard(
        validId<EffectGuardId>("guard.null"), nullptr).hasValue());
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
