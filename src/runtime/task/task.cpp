#include <lasercnc/runtime/task.hpp>

#include <utility>

namespace lasercnc::runtime {

CancellationToken::CancellationToken(std::shared_ptr<State> state) noexcept
    : state_(std::move(state))
{
}

bool CancellationToken::cancellationRequested() const noexcept
{
    return state_ != nullptr
        && (state_->requested.load(std::memory_order_acquire) || deadlineExceeded());
}

bool CancellationToken::deadlineExceeded() const noexcept
{
    return state_ != nullptr && state_->deadline.has_value()
        && std::chrono::steady_clock::now() >= *state_->deadline;
}

ProgressReporter::ProgressReporter(Callback callback)
    : callback_(std::move(callback))
{
}

foundation::Result<void> ProgressReporter::report(double completed, std::string message) const
{
    if(!callback_) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Task.ProgressReporterUnavailable",
            foundation::ErrorCategory::Conflict,
            "The progress reporter is not attached to a running task"));
    }
    return callback_(completed, std::move(message));
}

bool isTerminal(TaskState state) noexcept
{
    return state == TaskState::Succeeded || state == TaskState::Failed
        || state == TaskState::Cancelled || state == TaskState::Stale;
}

} // namespace lasercnc::runtime
