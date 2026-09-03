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

} // namespace lasercnc::runtime
