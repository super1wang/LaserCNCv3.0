#include <lasercnc/runtime/execution_contract.hpp>

namespace lasercnc::runtime {

std::string_view executionScopeName(ExecutionScope scope) noexcept
{
    switch(scope) {
    case ExecutionScope::Global: return "global";
    case ExecutionScope::Session: return "session";
    case ExecutionScope::Project: return "project";
    case ExecutionScope::Document: return "document";
    }
    return "unknown";
}

bool validExecutionScope(ExecutionScope scope) noexcept
{
    switch(scope) {
    case ExecutionScope::Global:
    case ExecutionScope::Session:
    case ExecutionScope::Project:
    case ExecutionScope::Document:
        return true;
    }
    return false;
}

bool validContractStatus(ContractStatus status) noexcept
{
    switch(status) {
    case ContractStatus::Active:
    case ContractStatus::Deprecated:
        return true;
    }
    return false;
}

bool contextMatchesScope(
    const ExecutionContext& context,
    ExecutionScope scope) noexcept
{
    if(!validExecutionScope(scope)) {
        return false;
    }
    switch(scope) {
    case ExecutionScope::Global:
    case ExecutionScope::Session:
        return !context.projectId.has_value() && !context.documentId.has_value();
    case ExecutionScope::Project:
        return context.projectId.has_value() && !context.documentId.has_value();
    case ExecutionScope::Document:
        return context.projectId.has_value() && context.documentId.has_value();
    }
    return false;
}

std::string_view replayPolicyName(ReplayPolicy policy) noexcept
{
    switch(policy) {
    case ReplayPolicy::Safe: return "safe";
    case ReplayPolicy::Idempotent: return "idempotent";
    case ReplayPolicy::ReconcileOnly: return "reconcile_only";
    case ReplayPolicy::Never: return "never";
    }
    return "unknown";
}

bool validReplayPolicy(ReplayPolicy policy) noexcept
{
    switch(policy) {
    case ReplayPolicy::Safe:
    case ReplayPolicy::Idempotent:
    case ReplayPolicy::ReconcileOnly:
    case ReplayPolicy::Never:
        return true;
    }
    return false;
}

std::string_view recoveryDispositionName(RecoveryDisposition disposition) noexcept
{
    switch(disposition) {
    case RecoveryDisposition::Completed: return "completed";
    case RecoveryDisposition::Interrupted: return "interrupted";
    case RecoveryDisposition::Indeterminate: return "indeterminate";
    case RecoveryDisposition::ReconcileRequired: return "reconcile_required";
    }
    return "unknown";
}

std::string_view externalEffectStateName(ExternalEffectState state) noexcept
{
    switch(state) {
    case ExternalEffectState::Executing: return "executing";
    case ExternalEffectState::Completed: return "completed";
    case ExternalEffectState::Interrupted: return "interrupted";
    case ExternalEffectState::Indeterminate: return "indeterminate";
    case ExternalEffectState::ReconcileRequired: return "reconcile_required";
    }
    return "unknown";
}

bool validExternalEffectState(ExternalEffectState state) noexcept
{
    switch(state) {
    case ExternalEffectState::Executing:
    case ExternalEffectState::Completed:
    case ExternalEffectState::Interrupted:
    case ExternalEffectState::Indeterminate:
    case ExternalEffectState::ReconcileRequired:
        return true;
    }
    return false;
}

RecoveryDisposition interruptedDisposition(ReplayPolicy policy) noexcept
{
    switch(policy) {
    case ReplayPolicy::Safe:
    case ReplayPolicy::Idempotent:
        return RecoveryDisposition::Interrupted;
    case ReplayPolicy::ReconcileOnly:
        return RecoveryDisposition::ReconcileRequired;
    case ReplayPolicy::Never:
        return RecoveryDisposition::Indeterminate;
    }
    return RecoveryDisposition::Indeterminate;
}

bool explicitRetryAllowed(ReplayPolicy policy) noexcept
{
    return policy == ReplayPolicy::Safe || policy == ReplayPolicy::Idempotent;
}

} // namespace lasercnc::runtime
