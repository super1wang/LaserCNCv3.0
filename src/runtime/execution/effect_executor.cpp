#include <lasercnc/runtime/effect_executor.hpp>

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/foundation/schema.hpp>
#include <lasercnc/persistence/persistence_service.hpp>
#include <lasercnc/runtime/effect_guard.hpp>
#include <lasercnc/runtime/resource_manager.hpp>
#include <lasercnc/state/document_store.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace lasercnc::runtime {
namespace {

bool isExternalSideEffect(SideEffectLevel sideEffect) noexcept
{
    return sideEffect != SideEffectLevel::ReadOnly
        && sideEffect != SideEffectLevel::DocumentWrite;
}

foundation::Error effectError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const CommandRequest& request,
    foundation::Value::Object details = {},
    std::shared_ptr<const foundation::Error> cause = nullptr)
{
    details.emplace("command", foundation::Value {std::string(request.command.value())});
    details.emplace("requestId", foundation::Value {std::string(request.requestId.value())});
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {std::move(details)},
        foundation::Severity::Error,
        std::move(cause));
}

foundation::Value versionValue(const foundation::Version& version)
{
    return foundation::Value {foundation::Value::Object {
        {"major", foundation::Value {static_cast<std::int64_t>(version.major)}},
        {"minor", foundation::Value {static_cast<std::int64_t>(version.minor)}},
        {"patch", foundation::Value {static_cast<std::int64_t>(version.patch)}},
    }};
}

foundation::Value effectSignature(
    const CommandRequest& request,
    const CommandDescriptor& descriptor)
{
    foundation::Value::Array guards;
    guards.reserve(descriptor.effectGuards.size());
    for(const auto& guard : descriptor.effectGuards) {
        guards.emplace_back(std::string(guard.value()));
    }
    foundation::Value::Array resources;
    resources.reserve(descriptor.resources.size());
    for(const auto& resource : descriptor.resources) {
        resources.emplace_back(foundation::Value::Object {
            {"access", foundation::Value {static_cast<std::int64_t>(resource.access)}},
            {"kind", foundation::Value {static_cast<std::int64_t>(resource.kind)}},
            {"resource", foundation::Value {std::string(resource.resource.value())}},
            {"units", foundation::Value {std::to_string(resource.units)}},
        });
    }
    return foundation::Value {foundation::Value::Object {
        {"arguments", request.arguments},
        {"command", foundation::Value {std::string(request.command.value())}},
        {"documentId", request.context.documentId.has_value()
            ? foundation::Value {std::string(request.context.documentId->value())}
            : foundation::Value {}},
        {"effectGuards", foundation::Value {std::move(guards)}},
        {"expectedRevision", request.expectedRevision.has_value()
            ? foundation::Value {std::to_string(request.expectedRevision->value())}
            : foundation::Value {}},
        {"format", foundation::Value {"lasercnc.external-effect-signature"}},
        {"projectId", request.context.projectId.has_value()
            ? foundation::Value {std::string(request.context.projectId->value())}
            : foundation::Value {}},
        {"replayPolicy", foundation::Value {
            std::string(replayPolicyName(descriptor.replayPolicy))}},
        {"requestedVersion", versionValue(request.version)},
        {"resolvedVersion", versionValue(descriptor.version)},
        {"resources", foundation::Value {std::move(resources)}},
        {"sessionId", foundation::Value {std::string(request.context.sessionId.value())}},
        {"sideEffect", foundation::Value {
            static_cast<std::int64_t>(descriptor.sideEffect)}},
        {"version", foundation::Value {std::int64_t {1}}},
        {"versionResolution", foundation::Value {
            static_cast<std::int64_t>(request.versionResolution)}},
    }};
}

template <typename Callback>
class ScopeExit final {
public:
    explicit ScopeExit(Callback callback) : callback_(std::move(callback)) {}
    ~ScopeExit() { callback_(); }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    Callback callback_;
};

foundation::Result<ExternalEffectExecutionResult> interruptedFailure(
    const CommandRequest& request,
    persistence::PersistenceService& persistence,
    const foundation::Value& signature,
    foundation::Error primary)
{
    auto disposition = persistence.interruptExternalEffect(
        *request.idempotencyKey, signature);
    foundation::Value::Object details;
    const char* code = "Effect.ExecutionStateUnknown";
    const char* message = "The external effect failed and its durable state is unknown";
    if(disposition) {
        details.emplace(
            "recoveryDisposition",
            foundation::Value {std::string(recoveryDispositionName(disposition.value()))});
        switch(disposition.value()) {
        case RecoveryDisposition::Interrupted:
            code = "Effect.ExecutionInterrupted";
            message = "The external effect failed and may be retried explicitly";
            break;
        case RecoveryDisposition::ReconcileRequired:
            code = "Effect.ReconcileRequired";
            message = "The external effect failed and requires explicit reconciliation";
            break;
        case RecoveryDisposition::Indeterminate:
            code = "Effect.Indeterminate";
            message = "The external effect failed with an indeterminate outcome";
            break;
        case RecoveryDisposition::Completed:
            code = "Effect.CompletionConflict";
            message = "The failed external effect was already recorded as completed";
            break;
        }
    } else {
        details.emplace(
            "persistenceErrorCode",
            foundation::Value {std::string(disposition.error().code.value())});
    }
    return foundation::Result<ExternalEffectExecutionResult>::failure(effectError(
        code,
        foundation::ErrorCategory::Infrastructure,
        message,
        request,
        std::move(details),
        std::make_shared<const foundation::Error>(std::move(primary))));
}

} // namespace

EffectExecutor::EffectExecutor(
    EffectGuardRegistry& guards,
    ResourceManager& resources,
    const state::DocumentStore& documents,
    persistence::PersistenceService& persistence) noexcept
    : guards_(guards),
      resources_(resources),
      documents_(documents),
      persistence_(persistence)
{
}

foundation::Result<void> EffectExecutor::validate(
    const std::vector<CommandDescriptor>& commands) const
{
    bool hasExternalEffects = false;
    for(const auto& command : commands) {
        if(!isExternalSideEffect(command.sideEffect)) {
            continue;
        }
        hasExternalEffects = true;
        std::set<kernel::EffectGuardId> uniqueGuards;
        for(const auto& id : command.effectGuards) {
            if(!uniqueGuards.insert(id).second) {
                return foundation::Result<void>::failure(foundation::makeError(
                    "EffectGuard.DuplicateRequirement",
                    foundation::ErrorCategory::Validation,
                    "An external-effect command declares the same guard more than once",
                    foundation::Value {foundation::Value::Object {
                        {"command", foundation::Value {std::string(command.name.value())}},
                        {"guardId", foundation::Value {std::string(id.value())}},
                    }}));
            }
            auto registered = guards_.guard(id);
            if(!registered) {
                return foundation::Result<void>::failure(foundation::makeError(
                    "EffectGuard.CommandRequirementMissing",
                    foundation::ErrorCategory::Validation,
                    "An external-effect command requires an unregistered guard",
                    foundation::Value {foundation::Value::Object {
                        {"command", foundation::Value {std::string(command.name.value())}},
                        {"guardId", foundation::Value {std::string(id.value())}},
                    }},
                    foundation::Severity::Error,
                    std::make_shared<const foundation::Error>(
                        std::move(registered).error())));
            }
        }
    }
    if(hasExternalEffects && !persistence_.configured()) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Effect.PersistenceRequired",
            foundation::ErrorCategory::Conflict,
            "External-effect commands require configured durable persistence"));
    }
    return foundation::Result<void>::success();
}

foundation::Result<ExternalEffectExecutionResult> EffectExecutor::execute(
    const CommandRequest& request,
    const CommandDescriptor& descriptor,
    const std::shared_ptr<IExternalEffectHandler>& handler,
    const foundation::ISchemaValidator& schemaValidator)
{
    if(handler == nullptr || !isExternalSideEffect(descriptor.sideEffect)) {
        return foundation::Result<ExternalEffectExecutionResult>::failure(effectError(
            "Effect.InvalidExecutionContract",
            foundation::ErrorCategory::Internal,
            "The external-effect execution contract is inconsistent",
            request));
    }
    if(!request.idempotencyKey.has_value()) {
        return foundation::Result<ExternalEffectExecutionResult>::failure(effectError(
            "Effect.IdempotencyKeyRequired",
            foundation::ErrorCategory::Validation,
            "External effects require a stable idempotency key",
            request));
    }
    if(!persistence_.ready()) {
        return foundation::Result<ExternalEffectExecutionResult>::failure(effectError(
            "Effect.PersistenceNotReady",
            foundation::ErrorCategory::Conflict,
            "Durable persistence must be ready before external effects execute",
            request));
    }

    EffectGuardContext guardContext;
    if(request.context.documentId.has_value()) {
        auto document = documents_.snapshot(*request.context.documentId);
        if(!document) {
            return foundation::Result<ExternalEffectExecutionResult>::failure(
                std::move(document).error());
        }
        if(document.value().projectId() != *request.context.projectId) {
            return foundation::Result<ExternalEffectExecutionResult>::failure(effectError(
                "Effect.ProjectMismatch",
                foundation::ErrorCategory::Validation,
                "The effect project does not own the target document",
                request));
        }
        if(request.expectedRevision.has_value()
           && document.value().revisions().at(state::RevisionScope::Project)
               != *request.expectedRevision) {
            return foundation::Result<ExternalEffectExecutionResult>::failure(effectError(
                "Effect.RevisionConflict",
                foundation::ErrorCategory::Conflict,
                "The effect project revision does not match the caller precondition",
                request));
        }
        guardContext.document = std::move(document).value();
    } else if(request.expectedRevision.has_value()) {
        return foundation::Result<ExternalEffectExecutionResult>::failure(effectError(
            "Effect.RevisionScopeMismatch",
            foundation::ErrorCategory::Validation,
            "Revision preconditions require document scope",
            request));
    }

    for(const auto& id : descriptor.effectGuards) {
        auto guard = guards_.guard(id);
        if(!guard) {
            return foundation::Result<ExternalEffectExecutionResult>::failure(
                std::move(guard).error());
        }
        foundation::Result<void> allowed = [&]() {
            try {
                return guard.value()->evaluate(request, descriptor, guardContext);
            } catch(const std::exception& exception) {
                return foundation::Result<void>::failure(effectError(
                    "Effect.GuardFailed",
                    foundation::ErrorCategory::Internal,
                    "An effect guard raised an exception",
                    request,
                    {{"guardId", foundation::Value {std::string(id.value())}},
                     {"reason", foundation::Value {exception.what()}}}));
            } catch(...) {
                return foundation::Result<void>::failure(effectError(
                    "Effect.GuardFailed",
                    foundation::ErrorCategory::Internal,
                    "An effect guard raised an exception",
                    request,
                    {{"guardId", foundation::Value {std::string(id.value())}}}));
            }
        }();
        if(!allowed) {
            return foundation::Result<ExternalEffectExecutionResult>::failure(
                std::move(allowed).error());
        }
    }

    auto acquired = resources_.tryAcquire(descriptor.resources);
    if(!acquired) {
        return foundation::Result<ExternalEffectExecutionResult>::failure(effectError(
            "Effect.ResourceClaimInvalid",
            foundation::ErrorCategory::Validation,
            "The external-effect resource claim is invalid",
            request,
            {},
            std::make_shared<const foundation::Error>(std::move(acquired).error())));
    }
    if(!acquired.value()) {
        return foundation::Result<ExternalEffectExecutionResult>::failure(effectError(
            "Effect.ResourceBusy",
            foundation::ErrorCategory::Conflict,
            "The external-effect resources are currently unavailable",
            request));
    }
    ScopeExit releaseResources([&]() noexcept {
        resources_.release(descriptor.resources);
    });

    auto signature = effectSignature(request, descriptor);
    auto claim = persistence_.claimExternalEffect(
        *request.idempotencyKey, signature, descriptor.replayPolicy);
    if(!claim) {
        return foundation::Result<ExternalEffectExecutionResult>::failure(effectError(
            "Effect.DurableClaimFailed",
            foundation::ErrorCategory::Infrastructure,
            "The external effect could not acquire its durable execution record",
            request,
            {},
            std::make_shared<const foundation::Error>(std::move(claim).error())));
    }
    if(claim.value().disposition
       == persistence::ExternalEffectClaimDisposition::Replayed) {
        if(!claim.value().replay.has_value()) {
            return foundation::Result<ExternalEffectExecutionResult>::failure(effectError(
                "Effect.ReplayMissing",
                foundation::ErrorCategory::Infrastructure,
                "A completed external effect has no replayable outcome",
                request));
        }
        auto replayValid = schemaValidator.validate(
            descriptor.result, *claim.value().replay);
        if(!replayValid) {
            return foundation::Result<ExternalEffectExecutionResult>::failure(effectError(
                "Effect.ReplayInvalid",
                foundation::ErrorCategory::Infrastructure,
                "The durable external-effect outcome no longer satisfies its result schema",
                request,
                {},
                std::make_shared<const foundation::Error>(
                    std::move(replayValid).error())));
        }
        return foundation::Result<ExternalEffectExecutionResult>::success(
            ExternalEffectExecutionResult {
                std::move(*claim.value().replay),
                true,
                RecoveryDisposition::Completed});
    }

    ExternalEffectContext context {
        guardContext.document,
        ResourceContext {descriptor.resources},
        claim.value().resumed};
    foundation::Result<foundation::Value> handled = [&]() {
        try {
            return handler->execute(request, context);
        } catch(const std::exception& exception) {
            return foundation::Result<foundation::Value>::failure(effectError(
                "Effect.HandlerFailed",
                foundation::ErrorCategory::Internal,
                "The external-effect handler raised an exception",
                request,
                {{"reason", foundation::Value {exception.what()}}}));
        } catch(...) {
            return foundation::Result<foundation::Value>::failure(effectError(
                "Effect.HandlerFailed",
                foundation::ErrorCategory::Internal,
                "The external-effect handler raised an exception",
                request));
        }
    }();
    if(!handled) {
        return interruptedFailure(
            request, persistence_, signature, std::move(handled).error());
    }
    auto resultValid = schemaValidator.validate(descriptor.result, handled.value());
    if(!resultValid) {
        return interruptedFailure(
            request, persistence_, signature, std::move(resultValid).error());
    }
    auto completed = persistence_.completeExternalEffect(
        *request.idempotencyKey, signature, handled.value());
    if(!completed) {
        return interruptedFailure(
            request, persistence_, signature, std::move(completed).error());
    }
    return foundation::Result<ExternalEffectExecutionResult>::success(
        ExternalEffectExecutionResult {
            std::move(handled).value(),
            false,
            RecoveryDisposition::Completed});
}

} // namespace lasercnc::runtime
