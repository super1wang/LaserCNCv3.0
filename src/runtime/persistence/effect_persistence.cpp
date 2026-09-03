#include <lasercnc/persistence/persistence_service.hpp>

#include <lasercnc/foundation/error.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace lasercnc::persistence {
namespace {

foundation::Error effectPersistenceError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    foundation::Value::Object details = {},
    std::shared_ptr<const foundation::Error> cause = nullptr)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {std::move(details)},
        foundation::Severity::Error,
        std::move(cause));
}

std::span<const std::byte> bytes(std::string_view value) noexcept
{
    return {
        reinterpret_cast<const std::byte*>(value.data()),
        value.size()};
}

foundation::Result<void> rollback(
    platform::IPersistenceBackend& backend,
    foundation::Error primary)
{
    auto rolledBack = backend.rollbackTransaction();
    if(rolledBack) {
        return foundation::Result<void>::failure(std::move(primary));
    }
    auto rollbackError = std::move(rolledBack).error();
    return foundation::Result<void>::failure(effectPersistenceError(
        "Persistence.RollbackFailed",
        foundation::ErrorCategory::Infrastructure,
        "External-effect persistence failed and could not roll back",
        {{"rollbackCode", foundation::Value {std::string(rollbackError.code.value())}},
         {"rollbackMessage", foundation::Value {rollbackError.message}}},
        std::make_shared<const foundation::Error>(std::move(primary))));
}

foundation::Result<std::string> textColumn(
    const platform::PersistenceRow& row,
    const char* name)
{
    const auto found = row.find(name);
    const auto* value = found == row.end() ? nullptr : found->second.getIf<std::string>();
    if(value == nullptr) {
        return foundation::Result<std::string>::failure(effectPersistenceError(
            "Persistence.InvalidExternalEffectRow",
            foundation::ErrorCategory::Infrastructure,
            "An external-effect row contains a missing or invalid text column",
            {{"column", foundation::Value {name}}}));
    }
    return foundation::Result<std::string>::success(*value);
}

foundation::Result<std::int64_t> integerColumn(
    const platform::PersistenceRow& row,
    const char* name)
{
    const auto found = row.find(name);
    const auto* value = found == row.end() ? nullptr : found->second.getIf<std::int64_t>();
    if(value == nullptr || *value < 0) {
        return foundation::Result<std::int64_t>::failure(effectPersistenceError(
            "Persistence.InvalidExternalEffectRow",
            foundation::ErrorCategory::Infrastructure,
            "An external-effect row contains a missing or invalid integer column",
            {{"column", foundation::Value {name}}}));
    }
    return foundation::Result<std::int64_t>::success(*value);
}

foundation::Result<runtime::ReplayPolicy> parsePolicy(std::string_view value)
{
    if(value == "safe") {
        return foundation::Result<runtime::ReplayPolicy>::success(
            runtime::ReplayPolicy::Safe);
    }
    if(value == "idempotent") {
        return foundation::Result<runtime::ReplayPolicy>::success(
            runtime::ReplayPolicy::Idempotent);
    }
    if(value == "reconcile_only") {
        return foundation::Result<runtime::ReplayPolicy>::success(
            runtime::ReplayPolicy::ReconcileOnly);
    }
    if(value == "never") {
        return foundation::Result<runtime::ReplayPolicy>::success(
            runtime::ReplayPolicy::Never);
    }
    return foundation::Result<runtime::ReplayPolicy>::failure(effectPersistenceError(
        "Persistence.InvalidExternalEffectPolicy",
        foundation::ErrorCategory::Infrastructure,
        "An external-effect row contains an invalid replay policy"));
}

foundation::Result<runtime::ExternalEffectState> parseState(std::string_view value)
{
    if(value == "executing") {
        return foundation::Result<runtime::ExternalEffectState>::success(
            runtime::ExternalEffectState::Executing);
    }
    if(value == "completed") {
        return foundation::Result<runtime::ExternalEffectState>::success(
            runtime::ExternalEffectState::Completed);
    }
    if(value == "interrupted") {
        return foundation::Result<runtime::ExternalEffectState>::success(
            runtime::ExternalEffectState::Interrupted);
    }
    if(value == "indeterminate") {
        return foundation::Result<runtime::ExternalEffectState>::success(
            runtime::ExternalEffectState::Indeterminate);
    }
    if(value == "reconcile_required") {
        return foundation::Result<runtime::ExternalEffectState>::success(
            runtime::ExternalEffectState::ReconcileRequired);
    }
    return foundation::Result<runtime::ExternalEffectState>::failure(effectPersistenceError(
        "Persistence.InvalidExternalEffectState",
        foundation::ErrorCategory::Infrastructure,
        "An external-effect row contains an invalid execution state"));
}

bool stateMatchesPolicy(
    runtime::ExternalEffectState state,
    runtime::ReplayPolicy policy) noexcept
{
    switch(state) {
    case runtime::ExternalEffectState::Executing:
    case runtime::ExternalEffectState::Completed:
        return true;
    case runtime::ExternalEffectState::Interrupted:
        return runtime::explicitRetryAllowed(policy);
    case runtime::ExternalEffectState::ReconcileRequired:
        return policy == runtime::ReplayPolicy::ReconcileOnly;
    case runtime::ExternalEffectState::Indeterminate:
        return policy == runtime::ReplayPolicy::Never;
    }
    return false;
}

foundation::Result<void> validateStatePolicy(
    runtime::ExternalEffectState state,
    runtime::ReplayPolicy policy)
{
    if(stateMatchesPolicy(state, policy)) {
        return foundation::Result<void>::success();
    }
    return foundation::Result<void>::failure(effectPersistenceError(
        "Persistence.ExternalEffectStatePolicyMismatch",
        foundation::ErrorCategory::Infrastructure,
        "An external-effect state is inconsistent with its replay policy"));
}

foundation::Result<void> verifyDigest(
    const platform::IHashService& hashes,
    std::string_view payload,
    std::string_view expected)
{
    auto digest = hashes.digest(bytes(payload));
    if(!digest) {
        return foundation::Result<void>::failure(std::move(digest).error());
    }
    if(digest.value().value() != expected) {
        return foundation::Result<void>::failure(effectPersistenceError(
            "Persistence.ExternalEffectDigestMismatch",
            foundation::ErrorCategory::Infrastructure,
            "An external-effect payload failed its content digest check"));
    }
    return foundation::Result<void>::success();
}

foundation::Result<std::optional<std::string>> optionalTextColumn(
    const platform::PersistenceRow& row,
    const char* name)
{
    const auto found = row.find(name);
    if(found == row.end()) {
        return foundation::Result<std::optional<std::string>>::failure(
            effectPersistenceError(
                "Persistence.InvalidExternalEffectRow",
                foundation::ErrorCategory::Infrastructure,
                "An external-effect row is missing a nullable column",
                {{"column", foundation::Value {name}}}));
    }
    if(found->second.kind() == foundation::Value::Kind::Null) {
        return foundation::Result<std::optional<std::string>>::success(std::nullopt);
    }
    const auto* value = found->second.getIf<std::string>();
    if(value == nullptr) {
        return foundation::Result<std::optional<std::string>>::failure(
            effectPersistenceError(
                "Persistence.InvalidExternalEffectRow",
                foundation::ErrorCategory::Infrastructure,
                "An external-effect row contains an invalid nullable text column",
                {{"column", foundation::Value {name}}}));
    }
    return foundation::Result<std::optional<std::string>>::success(*value);
}

foundation::Result<void> verifySignature(
    const platform::PersistenceRow& row,
    const std::string& payload,
    const kernel::ContentDigest& digest,
    runtime::ReplayPolicy replayPolicy,
    const platform::IHashService& hashes)
{
    auto storedPayload = textColumn(row, "signature_payload");
    auto storedDigest = textColumn(row, "signature_digest");
    auto storedPolicyText = textColumn(row, "replay_policy");
    if(!storedPayload || !storedDigest || !storedPolicyText) {
        return foundation::Result<void>::failure(effectPersistenceError(
            "Persistence.InvalidExternalEffectRow",
            foundation::ErrorCategory::Infrastructure,
            "An external-effect identity row is incomplete"));
    }
    auto intact = verifyDigest(hashes, storedPayload.value(), storedDigest.value());
    auto storedPolicy = parsePolicy(storedPolicyText.value());
    if(!intact || !storedPolicy) {
        return foundation::Result<void>::failure(
            !intact ? std::move(intact).error() : std::move(storedPolicy).error());
    }
    if(storedPayload.value() != payload || storedDigest.value() != digest.value()
       || storedPolicy.value() != replayPolicy) {
        return foundation::Result<void>::failure(effectPersistenceError(
            "Persistence.ExternalEffectIdentityConflict",
            foundation::ErrorCategory::Conflict,
            "The idempotency key is already bound to a different external effect"));
    }
    return foundation::Result<void>::success();
}

foundation::Result<foundation::Value> loadOutcome(
    const platform::PersistenceRow& row,
    const foundation::IValueSerializer& serializer,
    const platform::IHashService& hashes)
{
    auto payload = optionalTextColumn(row, "outcome_payload");
    auto digest = optionalTextColumn(row, "outcome_digest");
    if(!payload || !digest || !payload.value().has_value() || !digest.value().has_value()) {
        return foundation::Result<foundation::Value>::failure(effectPersistenceError(
            "Persistence.InvalidExternalEffectOutcome",
            foundation::ErrorCategory::Infrastructure,
            "A completed external effect has no valid outcome material"));
    }
    auto intact = verifyDigest(hashes, *payload.value(), *digest.value());
    if(!intact) {
        return foundation::Result<foundation::Value>::failure(std::move(intact).error());
    }
    return serializer.deserialize(*payload.value());
}

foundation::Result<kernel::ContentDigest> digestPayload(
    const platform::IHashService& hashes,
    const std::string& payload)
{
    return hashes.digest(bytes(payload));
}

std::int64_t nowMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

runtime::ExternalEffectState interruptedState(runtime::ReplayPolicy policy) noexcept
{
    switch(runtime::interruptedDisposition(policy)) {
    case runtime::RecoveryDisposition::Interrupted:
        return runtime::ExternalEffectState::Interrupted;
    case runtime::RecoveryDisposition::ReconcileRequired:
        return runtime::ExternalEffectState::ReconcileRequired;
    case runtime::RecoveryDisposition::Indeterminate:
        return runtime::ExternalEffectState::Indeterminate;
    case runtime::RecoveryDisposition::Completed:
        break;
    }
    return runtime::ExternalEffectState::Indeterminate;
}

foundation::Result<runtime::RecoveryDisposition> dispositionOf(
    runtime::ExternalEffectState state)
{
    switch(state) {
    case runtime::ExternalEffectState::Completed:
        return foundation::Result<runtime::RecoveryDisposition>::success(
            runtime::RecoveryDisposition::Completed);
    case runtime::ExternalEffectState::Interrupted:
        return foundation::Result<runtime::RecoveryDisposition>::success(
            runtime::RecoveryDisposition::Interrupted);
    case runtime::ExternalEffectState::Indeterminate:
        return foundation::Result<runtime::RecoveryDisposition>::success(
            runtime::RecoveryDisposition::Indeterminate);
    case runtime::ExternalEffectState::ReconcileRequired:
        return foundation::Result<runtime::RecoveryDisposition>::success(
            runtime::RecoveryDisposition::ReconcileRequired);
    case runtime::ExternalEffectState::Executing:
        break;
    }
    return foundation::Result<runtime::RecoveryDisposition>::failure(
        effectPersistenceError(
            "Persistence.ExternalEffectStillExecuting",
            foundation::ErrorCategory::Conflict,
            "An executing external effect has no recovery disposition"));
}

} // namespace

foundation::Result<ExternalEffectClaim> PersistenceService::claimExternalEffect(
    const kernel::IdempotencyKey& key,
    const foundation::Value& signature,
    runtime::ReplayPolicy replayPolicy)
{
    if(!runtime::validReplayPolicy(replayPolicy)) {
        return foundation::Result<ExternalEffectClaim>::failure(effectPersistenceError(
            "Persistence.InvalidExternalEffectPolicy",
            foundation::ErrorCategory::Validation,
            "The external-effect replay policy is invalid"));
    }
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<ExternalEffectClaim>::failure(effectPersistenceError(
            "Persistence.NotReady",
            foundation::ErrorCategory::Conflict,
            "Persistence must be initialized before external effects execute"));
    }
    bool transactionOpen = false;
    try {
        auto payload = serializer_->serialize(signature);
        if(!payload) {
            return foundation::Result<ExternalEffectClaim>::failure(
                std::move(payload).error());
        }
        auto digest = digestPayload(*hashes_, payload.value());
        if(!digest) {
            return foundation::Result<ExternalEffectClaim>::failure(
                std::move(digest).error());
        }
        const auto now = nowMilliseconds();
        if(now < 0) {
            return foundation::Result<ExternalEffectClaim>::failure(effectPersistenceError(
                "Persistence.InvalidTimestamp",
                foundation::ErrorCategory::Internal,
                "The system clock produced an unsupported external-effect timestamp"));
        }
        auto begun = backend_->beginTransaction();
        if(!begun) {
            return foundation::Result<ExternalEffectClaim>::failure(
                std::move(begun).error());
        }
        transactionOpen = true;
        const std::array queryParameters {foundation::Value {std::string(key.value())}};
        auto rows = backend_->query(
            "SELECT signature_payload,signature_digest,replay_policy,state,"
            "outcome_payload,outcome_digest FROM external_effects WHERE idempotency_key=?",
            queryParameters);
        if(!rows) {
            auto failure = rollback(*backend_, std::move(rows).error());
            return foundation::Result<ExternalEffectClaim>::failure(
                std::move(failure).error());
        }
        ExternalEffectClaim claim;
        if(rows.value().empty()) {
            const std::array insertParameters {
                foundation::Value {std::string(key.value())},
                foundation::Value {payload.value()},
                foundation::Value {std::string(digest.value().value())},
                foundation::Value {std::string(runtime::replayPolicyName(replayPolicy))},
                foundation::Value {now},
                foundation::Value {now}};
            auto inserted = backend_->execute(
                "INSERT INTO external_effects(idempotency_key,signature_payload,"
                "signature_digest,replay_policy,state,started_at_ms,updated_at_ms) "
                "VALUES(?,?,?,?,'executing',?,?)",
                insertParameters);
            if(!inserted || inserted.value() != 1U) {
                auto failure = rollback(
                    *backend_,
                    inserted ? effectPersistenceError(
                                   "Persistence.ExternalEffectClaimConflict",
                                   foundation::ErrorCategory::Conflict,
                                   "The external-effect identity changed while being claimed")
                             : std::move(inserted).error());
                return foundation::Result<ExternalEffectClaim>::failure(
                    std::move(failure).error());
            }
        } else if(rows.value().size() == 1U) {
            const auto& row = rows.value().front();
            auto identity = verifySignature(
                row, payload.value(), digest.value(), replayPolicy, *hashes_);
            auto stateText = textColumn(row, "state");
            auto state = stateText
                ? parseState(stateText.value())
                : foundation::Result<runtime::ExternalEffectState>::failure(
                      std::move(stateText).error());
            if(!identity || !state) {
                auto failure = rollback(
                    *backend_, !identity ? std::move(identity).error()
                                         : std::move(state).error());
                return foundation::Result<ExternalEffectClaim>::failure(
                    std::move(failure).error());
            }
            auto coherent = validateStatePolicy(state.value(), replayPolicy);
            if(!coherent) {
                auto failure = rollback(*backend_, std::move(coherent).error());
                return foundation::Result<ExternalEffectClaim>::failure(
                    std::move(failure).error());
            }
            if(state.value() == runtime::ExternalEffectState::Completed) {
                auto outcome = loadOutcome(row, *serializer_, *hashes_);
                if(!outcome) {
                    auto failure = rollback(*backend_, std::move(outcome).error());
                    return foundation::Result<ExternalEffectClaim>::failure(
                        std::move(failure).error());
                }
                claim.disposition = ExternalEffectClaimDisposition::Replayed;
                claim.replay = std::move(outcome).value();
            } else if(state.value() == runtime::ExternalEffectState::Interrupted
                      && runtime::explicitRetryAllowed(replayPolicy)) {
                const std::array updateParameters {
                    foundation::Value {now},
                    foundation::Value {std::string(key.value())}};
                auto updated = backend_->execute(
                    "UPDATE external_effects SET state='executing',updated_at_ms=? "
                    "WHERE idempotency_key=? AND state='interrupted'",
                    updateParameters);
                if(!updated || updated.value() != 1U) {
                    auto failure = rollback(
                        *backend_,
                        updated ? effectPersistenceError(
                                      "Persistence.ExternalEffectClaimConflict",
                                      foundation::ErrorCategory::Conflict,
                                      "The interrupted external effect changed before retry")
                                : std::move(updated).error());
                    return foundation::Result<ExternalEffectClaim>::failure(
                        std::move(failure).error());
                }
                claim.resumed = true;
            } else {
                const char* code = state.value() == runtime::ExternalEffectState::Executing
                    ? "Persistence.ExternalEffectAlreadyExecuting"
                    : state.value() == runtime::ExternalEffectState::ReconcileRequired
                    ? "Persistence.ExternalEffectReconcileRequired"
                    : "Persistence.ExternalEffectIndeterminate";
                auto failure = rollback(*backend_, effectPersistenceError(
                    code,
                    foundation::ErrorCategory::Conflict,
                    "The external effect is not eligible for execution"));
                return foundation::Result<ExternalEffectClaim>::failure(
                    std::move(failure).error());
            }
        } else {
            auto failure = rollback(*backend_, effectPersistenceError(
                "Persistence.InvalidExternalEffectRow",
                foundation::ErrorCategory::Infrastructure,
                "An external-effect identity resolved ambiguously"));
            return foundation::Result<ExternalEffectClaim>::failure(
                std::move(failure).error());
        }
        auto committed = backend_->commitTransaction();
        if(!committed) {
            auto failure = rollback(*backend_, std::move(committed).error());
            return foundation::Result<ExternalEffectClaim>::failure(
                std::move(failure).error());
        }
        transactionOpen = false;
        return foundation::Result<ExternalEffectClaim>::success(std::move(claim));
    } catch(const std::exception& exception) {
        auto error = effectPersistenceError(
            "Persistence.ExternalEffectClaimFailed",
            foundation::ErrorCategory::Internal,
            "External-effect claim failed unexpectedly",
            {{"reason", foundation::Value {exception.what()}}});
        auto failure = transactionOpen ? rollback(*backend_, std::move(error))
                                       : foundation::Result<void>::failure(std::move(error));
        return foundation::Result<ExternalEffectClaim>::failure(
            std::move(failure).error());
    } catch(...) {
        auto error = effectPersistenceError(
            "Persistence.ExternalEffectClaimFailed",
            foundation::ErrorCategory::Internal,
            "External-effect claim failed unexpectedly");
        auto failure = transactionOpen ? rollback(*backend_, std::move(error))
                                       : foundation::Result<void>::failure(std::move(error));
        return foundation::Result<ExternalEffectClaim>::failure(
            std::move(failure).error());
    }
}

foundation::Result<void> PersistenceService::completeExternalEffect(
    const kernel::IdempotencyKey& key,
    const foundation::Value& signature,
    const foundation::Value& outcome)
{
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<void>::failure(effectPersistenceError(
            "Persistence.NotReady",
            foundation::ErrorCategory::Conflict,
            "Persistence must be initialized before completing external effects"));
    }
    bool transactionOpen = false;
    try {
        auto signaturePayload = serializer_->serialize(signature);
        auto outcomePayload = serializer_->serialize(outcome);
        if(!signaturePayload || !outcomePayload) {
            return foundation::Result<void>::failure(
                !signaturePayload ? std::move(signaturePayload).error()
                                  : std::move(outcomePayload).error());
        }
        auto signatureDigest = digestPayload(*hashes_, signaturePayload.value());
        auto outcomeDigest = digestPayload(*hashes_, outcomePayload.value());
        if(!signatureDigest || !outcomeDigest) {
            return foundation::Result<void>::failure(
                !signatureDigest ? std::move(signatureDigest).error()
                                 : std::move(outcomeDigest).error());
        }
        const auto now = nowMilliseconds();
        auto begun = backend_->beginTransaction();
        if(!begun) {
            return foundation::Result<void>::failure(std::move(begun).error());
        }
        transactionOpen = true;
        const std::array queryParameters {foundation::Value {std::string(key.value())}};
        auto rows = backend_->query(
            "SELECT signature_payload,signature_digest,replay_policy,state,"
            "outcome_payload,outcome_digest FROM external_effects WHERE idempotency_key=?",
            queryParameters);
        if(!rows || rows.value().size() != 1U) {
            auto failure = rollback(
                *backend_,
                rows ? effectPersistenceError(
                           "Persistence.ExternalEffectMissing",
                           foundation::ErrorCategory::NotFound,
                           "The external effect has no durable execution record")
                     : std::move(rows).error());
            return foundation::Result<void>::failure(std::move(failure).error());
        }
        const auto& row = rows.value().front();
        auto policyText = textColumn(row, "replay_policy");
        auto policy = policyText
            ? parsePolicy(policyText.value())
            : foundation::Result<runtime::ReplayPolicy>::failure(
                  std::move(policyText).error());
        if(!policy) {
            auto failure = rollback(*backend_, std::move(policy).error());
            return foundation::Result<void>::failure(std::move(failure).error());
        }
        auto identity = verifySignature(
            row,
            signaturePayload.value(),
            signatureDigest.value(),
            policy.value(),
            *hashes_);
        auto stateText = textColumn(row, "state");
        auto state = stateText
            ? parseState(stateText.value())
            : foundation::Result<runtime::ExternalEffectState>::failure(
                  std::move(stateText).error());
        if(!identity || !state) {
            auto failure = rollback(
                *backend_, !identity ? std::move(identity).error()
                                     : std::move(state).error());
            return foundation::Result<void>::failure(std::move(failure).error());
        }
        auto coherent = validateStatePolicy(state.value(), policy.value());
        if(!coherent) {
            auto failure = rollback(*backend_, std::move(coherent).error());
            return foundation::Result<void>::failure(std::move(failure).error());
        }
        if(state.value() == runtime::ExternalEffectState::Completed) {
            auto stored = loadOutcome(row, *serializer_, *hashes_);
            if(!stored || stored.value() != outcome) {
                auto failure = rollback(
                    *backend_, stored ? effectPersistenceError(
                                           "Persistence.ExternalEffectOutcomeConflict",
                                           foundation::ErrorCategory::Conflict,
                                           "The external effect already has a different outcome")
                                     : std::move(stored).error());
                return foundation::Result<void>::failure(std::move(failure).error());
            }
        } else if(state.value() == runtime::ExternalEffectState::Executing) {
            const std::array updateParameters {
                foundation::Value {outcomePayload.value()},
                foundation::Value {std::string(outcomeDigest.value().value())},
                foundation::Value {now},
                foundation::Value {std::string(key.value())}};
            auto updated = backend_->execute(
                "UPDATE external_effects SET state='completed',outcome_payload=?,"
                "outcome_digest=?,updated_at_ms=? WHERE idempotency_key=? "
                "AND state='executing'",
                updateParameters);
            if(!updated || updated.value() != 1U) {
                auto failure = rollback(
                    *backend_,
                    updated ? effectPersistenceError(
                                  "Persistence.ExternalEffectCompletionConflict",
                                  foundation::ErrorCategory::Conflict,
                                  "The external effect changed before completion")
                            : std::move(updated).error());
                return foundation::Result<void>::failure(std::move(failure).error());
            }
        } else {
            auto failure = rollback(*backend_, effectPersistenceError(
                "Persistence.ExternalEffectCompletionConflict",
                foundation::ErrorCategory::Conflict,
                "Only an executing external effect may be completed"));
            return foundation::Result<void>::failure(std::move(failure).error());
        }
        auto committed = backend_->commitTransaction();
        if(!committed) {
            return rollback(*backend_, std::move(committed).error());
        }
        transactionOpen = false;
        return foundation::Result<void>::success();
    } catch(const std::exception& exception) {
        auto error = effectPersistenceError(
            "Persistence.ExternalEffectCompletionFailed",
            foundation::ErrorCategory::Internal,
            "External-effect completion failed unexpectedly",
            {{"reason", foundation::Value {exception.what()}}});
        return transactionOpen ? rollback(*backend_, std::move(error))
                               : foundation::Result<void>::failure(std::move(error));
    } catch(...) {
        auto error = effectPersistenceError(
            "Persistence.ExternalEffectCompletionFailed",
            foundation::ErrorCategory::Internal,
            "External-effect completion failed unexpectedly");
        return transactionOpen ? rollback(*backend_, std::move(error))
                               : foundation::Result<void>::failure(std::move(error));
    }
}

foundation::Result<runtime::RecoveryDisposition> PersistenceService::interruptExternalEffect(
    const kernel::IdempotencyKey& key,
    const foundation::Value& signature)
{
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<runtime::RecoveryDisposition>::failure(
            effectPersistenceError(
                "Persistence.NotReady",
                foundation::ErrorCategory::Conflict,
                "Persistence must be initialized before interrupting external effects"));
    }
    bool transactionOpen = false;
    try {
        auto signaturePayload = serializer_->serialize(signature);
        if(!signaturePayload) {
            return foundation::Result<runtime::RecoveryDisposition>::failure(
                std::move(signaturePayload).error());
        }
        auto signatureDigest = digestPayload(*hashes_, signaturePayload.value());
        if(!signatureDigest) {
            return foundation::Result<runtime::RecoveryDisposition>::failure(
                std::move(signatureDigest).error());
        }
        auto begun = backend_->beginTransaction();
        if(!begun) {
            return foundation::Result<runtime::RecoveryDisposition>::failure(
                std::move(begun).error());
        }
        transactionOpen = true;
        const std::array queryParameters {foundation::Value {std::string(key.value())}};
        auto rows = backend_->query(
            "SELECT signature_payload,signature_digest,replay_policy,state "
            "FROM external_effects WHERE idempotency_key=?",
            queryParameters);
        if(!rows || rows.value().size() != 1U) {
            auto failure = rollback(
                *backend_,
                rows ? effectPersistenceError(
                           "Persistence.ExternalEffectMissing",
                           foundation::ErrorCategory::NotFound,
                           "The external effect has no durable execution record")
                     : std::move(rows).error());
            return foundation::Result<runtime::RecoveryDisposition>::failure(
                std::move(failure).error());
        }
        const auto& row = rows.value().front();
        auto policyText = textColumn(row, "replay_policy");
        auto policy = policyText
            ? parsePolicy(policyText.value())
            : foundation::Result<runtime::ReplayPolicy>::failure(
                  std::move(policyText).error());
        if(!policy) {
            auto failure = rollback(*backend_, std::move(policy).error());
            return foundation::Result<runtime::RecoveryDisposition>::failure(
                std::move(failure).error());
        }
        auto identity = verifySignature(
            row,
            signaturePayload.value(),
            signatureDigest.value(),
            policy.value(),
            *hashes_);
        auto stateText = textColumn(row, "state");
        auto state = stateText
            ? parseState(stateText.value())
            : foundation::Result<runtime::ExternalEffectState>::failure(
                  std::move(stateText).error());
        if(!identity || !state) {
            auto failure = rollback(
                *backend_, !identity ? std::move(identity).error()
                                     : std::move(state).error());
            return foundation::Result<runtime::RecoveryDisposition>::failure(
                std::move(failure).error());
        }
        auto coherent = validateStatePolicy(state.value(), policy.value());
        if(!coherent) {
            auto failure = rollback(*backend_, std::move(coherent).error());
            return foundation::Result<runtime::RecoveryDisposition>::failure(
                std::move(failure).error());
        }
        auto target = state.value();
        if(state.value() == runtime::ExternalEffectState::Executing) {
            target = interruptedState(policy.value());
            const auto now = nowMilliseconds();
            const std::array updateParameters {
                foundation::Value {std::string(runtime::externalEffectStateName(target))},
                foundation::Value {now},
                foundation::Value {std::string(key.value())}};
            auto updated = backend_->execute(
                "UPDATE external_effects SET state=?,updated_at_ms=? "
                "WHERE idempotency_key=? AND state='executing'",
                updateParameters);
            if(!updated || updated.value() != 1U) {
                auto failure = rollback(
                    *backend_,
                    updated ? effectPersistenceError(
                                  "Persistence.ExternalEffectInterruptConflict",
                                  foundation::ErrorCategory::Conflict,
                                  "The external effect changed before interruption")
                            : std::move(updated).error());
                return foundation::Result<runtime::RecoveryDisposition>::failure(
                    std::move(failure).error());
            }
        }
        auto disposition = dispositionOf(target);
        if(!disposition) {
            auto failure = rollback(*backend_, std::move(disposition).error());
            return foundation::Result<runtime::RecoveryDisposition>::failure(
                std::move(failure).error());
        }
        auto committed = backend_->commitTransaction();
        if(!committed) {
            auto failure = rollback(*backend_, std::move(committed).error());
            return foundation::Result<runtime::RecoveryDisposition>::failure(
                std::move(failure).error());
        }
        transactionOpen = false;
        return disposition;
    } catch(const std::exception& exception) {
        auto error = effectPersistenceError(
            "Persistence.ExternalEffectInterruptFailed",
            foundation::ErrorCategory::Internal,
            "External-effect interruption failed unexpectedly",
            {{"reason", foundation::Value {exception.what()}}});
        auto failure = transactionOpen ? rollback(*backend_, std::move(error))
                                       : foundation::Result<void>::failure(std::move(error));
        return foundation::Result<runtime::RecoveryDisposition>::failure(
            std::move(failure).error());
    } catch(...) {
        auto error = effectPersistenceError(
            "Persistence.ExternalEffectInterruptFailed",
            foundation::ErrorCategory::Internal,
            "External-effect interruption failed unexpectedly");
        auto failure = transactionOpen ? rollback(*backend_, std::move(error))
                                       : foundation::Result<void>::failure(std::move(error));
        return foundation::Result<runtime::RecoveryDisposition>::failure(
            std::move(failure).error());
    }
}

foundation::Result<std::optional<ExternalEffectRecord>> PersistenceService::externalEffect(
    const kernel::IdempotencyKey& key) const
{
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<std::optional<ExternalEffectRecord>>::failure(
            effectPersistenceError(
                "Persistence.NotReady",
                foundation::ErrorCategory::Conflict,
                "Persistence must be initialized before reading external effects"));
    }
    try {
        const std::array parameters {foundation::Value {std::string(key.value())}};
        auto rows = backend_->query(
            "SELECT signature_payload,signature_digest,replay_policy,state,"
            "outcome_payload,outcome_digest,started_at_ms,updated_at_ms "
            "FROM external_effects WHERE idempotency_key=?",
            parameters);
        if(!rows) {
            return foundation::Result<std::optional<ExternalEffectRecord>>::failure(
                std::move(rows).error());
        }
        if(rows.value().empty()) {
            return foundation::Result<std::optional<ExternalEffectRecord>>::success(
                std::nullopt);
        }
        if(rows.value().size() != 1U) {
            return foundation::Result<std::optional<ExternalEffectRecord>>::failure(
                effectPersistenceError(
                    "Persistence.InvalidExternalEffectRow",
                    foundation::ErrorCategory::Infrastructure,
                    "An external-effect identity resolved ambiguously"));
        }
        const auto& row = rows.value().front();
        auto signaturePayload = textColumn(row, "signature_payload");
        auto signatureDigest = textColumn(row, "signature_digest");
        auto policyText = textColumn(row, "replay_policy");
        auto stateText = textColumn(row, "state");
        auto startedAt = integerColumn(row, "started_at_ms");
        auto updatedAt = integerColumn(row, "updated_at_ms");
        if(!signaturePayload || !signatureDigest || !policyText || !stateText
           || !startedAt || !updatedAt) {
            return foundation::Result<std::optional<ExternalEffectRecord>>::failure(
                effectPersistenceError(
                    "Persistence.InvalidExternalEffectRow",
                    foundation::ErrorCategory::Infrastructure,
                    "An external-effect record is incomplete"));
        }
        auto intact = verifyDigest(*hashes_, signaturePayload.value(), signatureDigest.value());
        auto policy = parsePolicy(policyText.value());
        auto state = parseState(stateText.value());
        if(!intact || !policy || !state) {
            return foundation::Result<std::optional<ExternalEffectRecord>>::failure(
                !intact ? std::move(intact).error()
                        : !policy ? std::move(policy).error() : std::move(state).error());
        }
        auto coherent = validateStatePolicy(state.value(), policy.value());
        if(!coherent) {
            return foundation::Result<std::optional<ExternalEffectRecord>>::failure(
                std::move(coherent).error());
        }
        std::optional<foundation::Value> outcome;
        if(state.value() == runtime::ExternalEffectState::Completed) {
            auto loaded = loadOutcome(row, *serializer_, *hashes_);
            if(!loaded) {
                return foundation::Result<std::optional<ExternalEffectRecord>>::failure(
                    std::move(loaded).error());
            }
            outcome = std::move(loaded).value();
        } else {
            auto outcomePayload = optionalTextColumn(row, "outcome_payload");
            auto outcomeDigest = optionalTextColumn(row, "outcome_digest");
            if(!outcomePayload || !outcomeDigest
               || outcomePayload.value().has_value()
               || outcomeDigest.value().has_value()) {
                return foundation::Result<std::optional<ExternalEffectRecord>>::failure(
                    effectPersistenceError(
                        "Persistence.InvalidExternalEffectOutcome",
                        foundation::ErrorCategory::Infrastructure,
                        "An unfinished external effect contains terminal outcome material"));
            }
        }
        return foundation::Result<std::optional<ExternalEffectRecord>>::success(
            ExternalEffectRecord {
                key,
                policy.value(),
                state.value(),
                std::move(outcome),
                std::chrono::system_clock::time_point {
                    std::chrono::milliseconds {startedAt.value()}},
                std::chrono::system_clock::time_point {
                    std::chrono::milliseconds {updatedAt.value()}}});
    } catch(const std::exception& exception) {
        return foundation::Result<std::optional<ExternalEffectRecord>>::failure(
            effectPersistenceError(
                "Persistence.ExternalEffectReadFailed",
                foundation::ErrorCategory::Internal,
                "External-effect read failed unexpectedly",
                {{"reason", foundation::Value {exception.what()}}}));
    } catch(...) {
        return foundation::Result<std::optional<ExternalEffectRecord>>::failure(
            effectPersistenceError(
                "Persistence.ExternalEffectReadFailed",
                foundation::ErrorCategory::Internal,
                "External-effect read failed unexpectedly"));
    }
}

} // namespace lasercnc::persistence
