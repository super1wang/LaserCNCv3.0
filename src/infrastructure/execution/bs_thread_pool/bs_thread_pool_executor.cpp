#include <lasercnc/infrastructure/bs_thread_pool_executor.hpp>

#include <lasercnc/foundation/error.hpp>

#include <BS_thread_pool.hpp>

#include <condition_variable>
#include <exception>
#include <mutex>
#include <string>
#include <utility>

namespace lasercnc::infrastructure {
namespace {

foundation::Error executorError(const char* code, const char* message, std::string reason)
{
    return foundation::makeError(
        code,
        foundation::ErrorCategory::Infrastructure,
        message,
        foundation::Value {foundation::Value::Object {
            {"backend", foundation::Value {"BS::thread_pool"}},
            {"reason", foundation::Value {std::move(reason)}},
        }});
}

foundation::Error validationError(const char* code, const char* message)
{
    return foundation::makeError(code, foundation::ErrorCategory::Validation, message);
}

foundation::Error stateConflict(const char* code, const char* message)
{
    return foundation::makeError(
        code,
        foundation::ErrorCategory::Conflict,
        message,
        foundation::Value {foundation::Value::Object {
            {"backend", foundation::Value {"BS::thread_pool"}},
        }});
}

foundation::Result<void> runWork(platform::ExecutorWork& work) noexcept
{
    try {
        return work();
    } catch(const std::exception& exception) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Execution.WorkThrew",
            foundation::ErrorCategory::Internal,
            "Executor work threw an exception",
            foundation::Value {foundation::Value::Object {
                {"reason", foundation::Value {exception.what()}},
            }}));
    } catch(...) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Execution.WorkThrew",
            foundation::ErrorCategory::Internal,
            "Executor work threw an unknown exception"));
    }
}

} // namespace

class BsThreadPoolExecutor::Impl final {
public:
    enum class State { Accepting, Draining, DrainFailed, Stopped };

    explicit Impl(std::size_t threadCount)
        : pool_(threadCount)
    {
    }

    [[nodiscard]] bool calledFromOwnWorker() const noexcept
    {
        const auto owner = BS::this_thread::get_pool();
        return owner.has_value() && *owner == &pool_;
    }

    BS::thread_pool<> pool_;
    mutable std::mutex stateMutex_;
    std::mutex waitMutex_;
    std::condition_variable stateChanged_;
    State state_{State::Accepting};
};

BsThreadPoolExecutor::BsThreadPoolExecutor(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation))
{
}

BsThreadPoolExecutor::~BsThreadPoolExecutor()
{
    drainForDestruction();
}

void BsThreadPoolExecutor::drainForDestruction() noexcept
{
    try {
        if(isCurrentWorkerThread() || !shutdown()) { std::terminate(); }
    } catch(...) {
        std::terminate();
    }
}

bool BsThreadPoolExecutor::isCurrentWorkerThread() const noexcept
{
    return implementation_->calledFromOwnWorker();
}

foundation::Result<std::unique_ptr<BsThreadPoolExecutor>> BsThreadPoolExecutor::create(
    BsThreadPoolExecutorOptions options)
{
    try {
        auto implementation = std::make_unique<Impl>(options.threadCount);
        return foundation::Result<std::unique_ptr<BsThreadPoolExecutor>>::success(
            std::unique_ptr<BsThreadPoolExecutor>(
                new BsThreadPoolExecutor(std::move(implementation))));
    } catch(const std::exception& exception) {
        return foundation::Result<std::unique_ptr<BsThreadPoolExecutor>>::failure(executorError(
            "Execution.InitializeFailed", "The task executor could not be initialized", exception.what()));
    } catch(...) {
        return foundation::Result<std::unique_ptr<BsThreadPoolExecutor>>::failure(executorError(
            "Execution.InitializeFailed", "The task executor could not be initialized", "Unknown failure"));
    }
}

foundation::Result<void> BsThreadPoolExecutor::submit(
    platform::ExecutorWork work,
    platform::ExecutorCompletion completion)
{
    if(!work) {
        return foundation::Result<void>::failure(validationError(
            "Execution.InvalidWork", "Executor work must be callable"));
    }
    if(!completion) {
        return foundation::Result<void>::failure(validationError(
            "Execution.InvalidCompletion", "Executor completion must be callable"));
    }

    try {
        std::lock_guard lock(implementation_->stateMutex_);
        if(implementation_->state_ != Impl::State::Accepting) {
            return foundation::Result<void>::failure(stateConflict(
                "Execution.ExecutorStopped", "The executor no longer accepts work"));
        }
        implementation_->pool_.detach_task(
            [work = std::move(work), completion = std::move(completion)]() mutable noexcept {
                auto outcome = runWork(work);
                try {
                    completion(std::move(outcome));
                } catch(...) {
                    // Completion is a notification boundary. A client exception must not escape
                    // into the third-party worker loop or prevent later tasks from running.
                }
            });
        return foundation::Result<void>::success();
    } catch(const std::exception& exception) {
        return foundation::Result<void>::failure(executorError(
            "Execution.SubmitFailed", "The task executor could not submit work", exception.what()));
    } catch(...) {
        return foundation::Result<void>::failure(executorError(
            "Execution.SubmitFailed", "The task executor could not submit work", "Unknown failure"));
    }
}

foundation::Result<void> BsThreadPoolExecutor::waitIdle()
{
    if(implementation_->calledFromOwnWorker()) {
        return foundation::Result<void>::failure(stateConflict(
            "Execution.WaitFromWorkerDenied",
            "A worker cannot wait for its own executor to become idle"));
    }
    try {
        std::lock_guard waitLock(implementation_->waitMutex_);
        implementation_->pool_.wait();
        return foundation::Result<void>::success();
    } catch(const std::exception& exception) {
        return foundation::Result<void>::failure(executorError(
            "Execution.WaitFailed", "The task executor could not wait for idle", exception.what()));
    } catch(...) {
        return foundation::Result<void>::failure(executorError(
            "Execution.WaitFailed", "The task executor could not wait for idle", "Unknown failure"));
    }
}

foundation::Result<void> BsThreadPoolExecutor::shutdown()
{
    if(implementation_->calledFromOwnWorker()) {
        return foundation::Result<void>::failure(stateConflict(
            "Execution.ShutdownFromWorkerDenied",
            "A worker cannot synchronously shut down its own executor"));
    }

    {
        std::unique_lock lock(implementation_->stateMutex_);
        while(implementation_->state_ == Impl::State::Draining) {
            implementation_->stateChanged_.wait(lock);
        }
        if(implementation_->state_ == Impl::State::Stopped) {
            return foundation::Result<void>::success();
        }
        implementation_->state_ = Impl::State::Draining;
    }

    try {
        std::lock_guard waitLock(implementation_->waitMutex_);
        implementation_->pool_.wait();
    } catch(const std::exception& exception) {
        {
            std::lock_guard lock(implementation_->stateMutex_);
            implementation_->state_ = Impl::State::DrainFailed;
        }
        implementation_->stateChanged_.notify_all();
        return foundation::Result<void>::failure(executorError(
            "Execution.ShutdownFailed", "The task executor could not shut down", exception.what()));
    } catch(...) {
        {
            std::lock_guard lock(implementation_->stateMutex_);
            implementation_->state_ = Impl::State::DrainFailed;
        }
        implementation_->stateChanged_.notify_all();
        return foundation::Result<void>::failure(executorError(
            "Execution.ShutdownFailed", "The task executor could not shut down", "Unknown failure"));
    }

    {
        std::lock_guard lock(implementation_->stateMutex_);
        implementation_->state_ = Impl::State::Stopped;
    }
    implementation_->stateChanged_.notify_all();
    return foundation::Result<void>::success();
}

std::size_t BsThreadPoolExecutor::concurrency() const noexcept
{
    return implementation_->pool_.get_thread_count();
}

} // namespace lasercnc::infrastructure
