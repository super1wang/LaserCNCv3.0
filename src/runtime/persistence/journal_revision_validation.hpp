#pragma once

#include <lasercnc/state/revision.hpp>

#include <array>
#include <limits>
#include <string>

namespace lasercnc::persistence::detail {

inline foundation::Result<void> validateJournalRevisionTransition(
    const state::RevisionSet& before, const state::RevisionSet& after, bool changesObjects,
    foundation::ErrorCategory category = foundation::ErrorCategory::Infrastructure)
{
    constexpr std::array scopes{state::RevisionScope::Project, state::RevisionScope::Document,
        state::RevisionScope::Geometry, state::RevisionScope::Cam,
        state::RevisionScope::MachineContext, state::RevisionScope::Environment};
    for(const auto scope : scopes) {
        const auto previous = before.at(scope).value();
        const auto next = after.at(scope).value();
        const bool advances = previous != std::numeric_limits<std::uint64_t>::max() && next == previous + 1U;
        const bool mustAdvance = changesObjects
            && (scope == state::RevisionScope::Project || scope == state::RevisionScope::Document);
        if((next != previous && !advances) || (mustAdvance && !advances)) {
            return foundation::Result<void>::failure(foundation::makeError(
                "Persistence.JournalRevisionTransitionInvalid", category,
                "Journal revisions must remain stable or advance once; object changes advance project and document",
                foundation::Value{foundation::Value::Object{
                    {"scope", foundation::Value{std::string(state::revisionScopeName(scope))}},
                    {"before", foundation::Value{std::to_string(previous)}},
                    {"after", foundation::Value{std::to_string(next)}}}}));
        }
    }
    return foundation::Result<void>::success();
}

} // namespace lasercnc::persistence::detail
