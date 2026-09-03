#include <lasercnc/runtime/script.hpp>

namespace lasercnc::runtime {

bool isTerminal(ScriptState state) noexcept
{
    return state == ScriptState::Succeeded || state == ScriptState::Failed
        || state == ScriptState::Cancelled;
}

} // namespace lasercnc::runtime
