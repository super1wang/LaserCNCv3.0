#include <lasercnc/kernel/app_kernel.hpp>

#include <lasercnc/foundation/error.hpp>

#include <string>
#include <utility>

namespace lasercnc::kernel {

AppKernel::AppKernel()
    : modules_(services_),
      transactions_(documents_),
      commands_(
          commandRegistry_, transactions_, capabilities_, events_, executionServices_),
      queries_(queryRegistry_, documents_, capabilities_, executionServices_),
      scheduler_(resources_),
      tasks_(taskRegistry_, scheduler_, executionServices_, documents_)
{
}

AppKernel::~AppKernel()
{
    if(state_ == AppKernelState::Ready || state_ == AppKernelState::Stopping) {
        static_cast<void>(shutdown());
    }
}

foundation::Result<void> AppKernel::addModule(std::unique_ptr<IModule> module)
{
    if(state_ != AppKernelState::Configuring) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.AppKernelNotConfiguring",
            foundation::ErrorCategory::Conflict,
            "Modules can only be added while the application kernel is configuring"));
    }
    return modules_.addModule(std::move(module));
}

foundation::Result<void> AppKernel::configureTaskExecutor(
    std::unique_ptr<platform::ITaskExecutor> executor)
{
    if(state_ != AppKernelState::Configuring) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.TaskExecutorNotConfiguring",
            foundation::ErrorCategory::Conflict,
            "The task executor can only be configured while the application kernel is configuring"));
    }
    if(executor == nullptr) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.InvalidTaskExecutor",
            foundation::ErrorCategory::Validation,
            "A task executor is required"));
    }
    auto configured = scheduler_.configureExecutor(*executor);
    if(!configured) {
        return configured;
    }
    taskExecutor_ = std::move(executor);
    return foundation::Result<void>::success();
}

foundation::Result<void> AppKernel::bootstrap()
{
    if(state_ != AppKernelState::Configuring) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.AppKernelAlreadyBootstrapped",
            foundation::ErrorCategory::Conflict,
            "The application kernel can only be bootstrapped once"));
    }

    state_ = AppKernelState::Starting;
    auto result = modules_.bootstrap(*this);
    if(!result) {
        state_ = AppKernelState::Failed;
        return result;
    }

    if((commandRegistry_.size() != 0U || queryRegistry_.size() != 0U
        || taskRegistry_.size() != 0U)
       && !executionServices_.configured()) {
        auto stopped = modules_.shutdown(*this);
        state_ = AppKernelState::Failed;
        return foundation::Result<void>::failure(foundation::makeError(
            "Runtime.ExecutionServicesNotConfigured",
            foundation::ErrorCategory::Conflict,
            "Registered commands and queries require schema validation and logging services",
            foundation::Value {},
            foundation::Severity::Error,
            stopped.hasValue()
                ? nullptr
                : std::make_shared<const foundation::Error>(std::move(stopped).error())));
    }
    if(taskRegistry_.size() != 0U && taskExecutor_ == nullptr) {
        auto stopped = modules_.shutdown(*this);
        state_ = AppKernelState::Failed;
        return foundation::Result<void>::failure(foundation::makeError(
            "Task.ExecutorNotConfigured",
            foundation::ErrorCategory::Conflict,
            "Registered tasks require a configured task executor",
            foundation::Value {},
            foundation::Severity::Error,
            stopped.hasValue()
                ? nullptr
                : std::make_shared<const foundation::Error>(std::move(stopped).error())));
    }

    if(taskExecutor_ != nullptr) {
        auto scheduled = scheduler_.start();
        if(!scheduled) {
            auto stopped = modules_.shutdown(*this);
            state_ = AppKernelState::Failed;
            return foundation::Result<void>::failure(foundation::makeError(
                "Task.SchedulerStartFailed",
                foundation::ErrorCategory::Infrastructure,
                "The task scheduler could not start",
                foundation::Value {},
                foundation::Severity::Error,
                std::make_shared<const foundation::Error>(std::move(scheduled).error())));
        }
    }

    services_.freeze();
    executionServices_.freeze();
    commandRegistry_.freeze();
    queryRegistry_.freeze();
    taskRegistry_.freeze();
    commands_.start();
    queries_.start();
    if(taskExecutor_ != nullptr) {
        tasks_.start();
    }
    state_ = AppKernelState::Ready;
    return foundation::Result<void>::success();
}

foundation::Result<void> AppKernel::shutdown(std::chrono::milliseconds taskTimeout)
{
    if(state_ == AppKernelState::Stopped) {
        return foundation::Result<void>::success();
    }
    if(state_ != AppKernelState::Ready && state_ != AppKernelState::Configuring
       && state_ != AppKernelState::Stopping) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.AppKernelCannotStop",
            foundation::ErrorCategory::Conflict,
            "The application kernel cannot stop from its current state"));
    }
    const auto activeTransactionCount = transactions_.activeTransactionCount();
    if(activeTransactionCount != 0U) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.ActiveTransactions",
            foundation::ErrorCategory::Conflict,
            "The application kernel cannot stop while transactions are active",
            foundation::Value {foundation::Value::Object {
                {"activeTransactionCount",
                 foundation::Value {std::to_string(activeTransactionCount)}},
            }}));
    }
    const auto activeCommandCount = commands_.activeExecutionCount();
    const auto activeQueryCount = queries_.activeExecutionCount();
    if(activeCommandCount != 0U || activeQueryCount != 0U) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.ActiveExecutions",
            foundation::ErrorCategory::Conflict,
            "The application kernel cannot stop while command or query executions are active",
            foundation::Value {foundation::Value::Object {
                {"activeCommandCount", foundation::Value {std::to_string(activeCommandCount)}},
                {"activeQueryCount", foundation::Value {std::to_string(activeQueryCount)}},
            }}));
    }

    const bool wasConfiguring = state_ == AppKernelState::Configuring;
    state_ = AppKernelState::Stopping;
    commands_.stop();
    queries_.stop();
    tasks_.stop();
    if(taskExecutor_ != nullptr) {
        auto tasksStopped = wasConfiguring ? taskExecutor_->shutdown()
                                           : scheduler_.shutdown(taskTimeout);
        if(!tasksStopped) {
            return tasksStopped;
        }
    }
    auto result = modules_.shutdown(*this);
    if(!result) {
        state_ = AppKernelState::Failed;
        return result;
    }

    state_ = AppKernelState::Stopped;
    return foundation::Result<void>::success();
}

ServiceRegistry& AppKernel::services() noexcept
{
    return services_;
}

const ServiceRegistry& AppKernel::services() const noexcept
{
    return services_;
}

ModuleRuntime& AppKernel::modules() noexcept
{
    return modules_;
}

const ModuleRuntime& AppKernel::modules() const noexcept
{
    return modules_;
}

foundation::Result<void> AppKernel::addDocument(
    ProjectId projectId,
    DocumentId documentId)
{
    if(state_ != AppKernelState::Configuring) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.DocumentLoadNotConfiguring",
            foundation::ErrorCategory::Conflict,
            "Documents can only be attached while the application kernel is configuring"));
    }
    return documents_.addDocument(std::move(projectId), std::move(documentId));
}

const state::DocumentStore& AppKernel::documents() const noexcept
{
    return documents_;
}

runtime::ExecutionServices& AppKernel::executionServices() noexcept
{
    return executionServices_;
}

const runtime::ExecutionServices& AppKernel::executionServices() const noexcept
{
    return executionServices_;
}

runtime::CapabilityService& AppKernel::capabilities() noexcept
{
    return capabilities_;
}

const runtime::CapabilityService& AppKernel::capabilities() const noexcept
{
    return capabilities_;
}

messaging::EventBus& AppKernel::events() noexcept
{
    return events_;
}

const messaging::EventBus& AppKernel::events() const noexcept
{
    return events_;
}

runtime::CommandRegistry& AppKernel::commandRegistry() noexcept
{
    return commandRegistry_;
}

const runtime::CommandRegistry& AppKernel::commandRegistry() const noexcept
{
    return commandRegistry_;
}

runtime::QueryRegistry& AppKernel::queryRegistry() noexcept
{
    return queryRegistry_;
}

const runtime::QueryRegistry& AppKernel::queryRegistry() const noexcept
{
    return queryRegistry_;
}

runtime::CommandRuntime& AppKernel::commands() noexcept
{
    return commands_;
}

const runtime::CommandRuntime& AppKernel::commands() const noexcept
{
    return commands_;
}

runtime::QueryRuntime& AppKernel::queries() noexcept
{
    return queries_;
}

const runtime::QueryRuntime& AppKernel::queries() const noexcept
{
    return queries_;
}

runtime::TaskRegistry& AppKernel::taskRegistry() noexcept
{
    return taskRegistry_;
}

const runtime::TaskRegistry& AppKernel::taskRegistry() const noexcept
{
    return taskRegistry_;
}

runtime::ResourceManager& AppKernel::resources() noexcept
{
    return resources_;
}

const runtime::ResourceManager& AppKernel::resources() const noexcept
{
    return resources_;
}

runtime::Scheduler& AppKernel::scheduler() noexcept
{
    return scheduler_;
}

const runtime::Scheduler& AppKernel::scheduler() const noexcept
{
    return scheduler_;
}

runtime::TaskRuntime& AppKernel::tasks() noexcept
{
    return tasks_;
}

const runtime::TaskRuntime& AppKernel::tasks() const noexcept
{
    return tasks_;
}

AppKernelState AppKernel::state() const noexcept
{
    return state_;
}

} // namespace lasercnc::kernel
