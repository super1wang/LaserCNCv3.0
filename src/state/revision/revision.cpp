#include <lasercnc/state/revision.hpp>

#include <lasercnc/foundation/error.hpp>

#include <array>
#include <exception>
#include <limits>
#include <string>

namespace lasercnc::state {
namespace {

constexpr std::size_t scopeIndex(RevisionScope scope) noexcept
{
    switch(scope) {
    case RevisionScope::Project: return 0U;
    case RevisionScope::Document: return 1U;
    case RevisionScope::Geometry: return 2U;
    case RevisionScope::Cam: return 3U;
    case RevisionScope::MachineContext: return 4U;
    case RevisionScope::Environment: return 5U;
    }
    std::terminate();
}

foundation::Error invalidScopeError()
{
    return foundation::makeError(
        "Revision.InvalidScope",
        foundation::ErrorCategory::Validation,
        "Revision scope is invalid");
}

foundation::Error revisionConflict(
    RevisionScope scope,
    Revision expected,
    Revision current)
{
    return foundation::makeError(
        "Project.RevisionConflict",
        foundation::ErrorCategory::Conflict,
        "The expected revision does not match the current revision",
        foundation::Value {foundation::Value::Object {
            {"scope", foundation::Value {std::string(revisionScopeName(scope))}},
            {"expected", foundation::Value {std::to_string(expected.value())}},
            {"current", foundation::Value {std::to_string(current.value())}},
        }});
}

bool isValidScope(RevisionScope scope) noexcept
{
    switch(scope) {
    case RevisionScope::Project:
    case RevisionScope::Document:
    case RevisionScope::Geometry:
    case RevisionScope::Cam:
    case RevisionScope::MachineContext:
    case RevisionScope::Environment:
        return true;
    }
    return false;
}

} // namespace

std::string_view revisionScopeName(RevisionScope scope) noexcept
{
    switch(scope) {
    case RevisionScope::Project: return "project";
    case RevisionScope::Document: return "document";
    case RevisionScope::Geometry: return "geometry";
    case RevisionScope::Cam: return "cam";
    case RevisionScope::MachineContext: return "machineContext";
    case RevisionScope::Environment: return "environment";
    }
    return "unknown";
}

foundation::Result<Revision> Revision::next() const
{
    if(value_ == std::numeric_limits<std::uint64_t>::max()) {
        return foundation::Result<Revision>::failure(foundation::makeError(
            "Revision.Overflow",
            foundation::ErrorCategory::Conflict,
            "Revision cannot be incremented beyond its maximum value"));
    }
    return foundation::Result<Revision>::success(Revision {value_ + 1U});
}

const Revision& RevisionSet::at(RevisionScope scope) const noexcept
{
    return values_[scopeIndex(scope)];
}

Revision& RevisionSet::atMutable(RevisionScope scope) noexcept
{
    return values_[scopeIndex(scope)];
}

foundation::Result<void> RevisionManager::validate(
    const RevisionSet& current,
    std::span<const RevisionPrecondition> preconditions)
{
    std::array<bool, RevisionSet::scopeCount> seen {};
    for(const auto& precondition : preconditions) {
        if(!isValidScope(precondition.scope)) {
            return foundation::Result<void>::failure(invalidScopeError());
        }
        const auto index = scopeIndex(precondition.scope);
        if(seen[index]) {
            return foundation::Result<void>::failure(foundation::makeError(
                "Revision.DuplicatePrecondition",
                foundation::ErrorCategory::Validation,
                "A revision scope may appear only once in preconditions",
                foundation::Value {foundation::Value::Object {
                    {"scope", foundation::Value {std::string(revisionScopeName(precondition.scope))}},
                }}));
        }
        seen[index] = true;
        if(current.at(precondition.scope) != precondition.expected) {
            return foundation::Result<void>::failure(revisionConflict(
                precondition.scope,
                precondition.expected,
                current.at(precondition.scope)));
        }
    }
    return foundation::Result<void>::success();
}

foundation::Result<RevisionSet> RevisionManager::advance(
    const RevisionSet& current,
    std::span<const RevisionScope> scopes)
{
    RevisionSet advanced = current;
    std::array<bool, RevisionSet::scopeCount> incremented {};
    for(const auto scope : scopes) {
        if(!isValidScope(scope)) {
            return foundation::Result<RevisionSet>::failure(invalidScopeError());
        }
        const auto index = scopeIndex(scope);
        if(incremented[index]) {
            continue;
        }
        auto nextRevision = current.at(scope).next();
        if(!nextRevision.hasValue()) {
            return foundation::Result<RevisionSet>::failure(std::move(nextRevision).error());
        }
        advanced.atMutable(scope) = std::move(nextRevision).value();
        incremented[index] = true;
    }
    return foundation::Result<RevisionSet>::success(std::move(advanced));
}

} // namespace lasercnc::state
