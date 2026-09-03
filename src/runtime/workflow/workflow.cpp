#include <lasercnc/runtime/workflow.hpp>

namespace lasercnc::runtime {

bool isTerminal(WorkflowState state) noexcept
{
    return state == WorkflowState::Succeeded || state == WorkflowState::Failed
        || state == WorkflowState::Cancelled || state == WorkflowState::Compensated
        || state == WorkflowState::CompensationFailed;
}

bool isTerminal(WorkflowStepState state) noexcept
{
    return state == WorkflowStepState::Succeeded || state == WorkflowStepState::Skipped
        || state == WorkflowStepState::Failed || state == WorkflowStepState::Cancelled
        || state == WorkflowStepState::Compensated
        || state == WorkflowStepState::CompensationFailed;
}

} // namespace lasercnc::runtime
