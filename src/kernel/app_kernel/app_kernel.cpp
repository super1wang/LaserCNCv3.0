#include <lasercnc/kernel/app_kernel.hpp>

#include <lasercnc/foundation/error.hpp>

#include <string>
#include <utility>

namespace lasercnc::kernel {

AppKernel::AppKernel()
    : modules_(services_), transactions_(documents_)
{
}

AppKernel::~AppKernel()
{
    if(state_ == AppKernelState::Ready) {
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

    services_.freeze();
    state_ = AppKernelState::Ready;
    return foundation::Result<void>::success();
}

foundation::Result<void> AppKernel::shutdown()
{
    if(state_ == AppKernelState::Stopped) {
        return foundation::Result<void>::success();
    }
    if(state_ != AppKernelState::Ready && state_ != AppKernelState::Configuring) {
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

    state_ = AppKernelState::Stopping;
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

state::DocumentStore& AppKernel::documents() noexcept
{
    return documents_;
}

const state::DocumentStore& AppKernel::documents() const noexcept
{
    return documents_;
}

runtime::TransactionManager& AppKernel::transactions() noexcept
{
    return transactions_;
}

const runtime::TransactionManager& AppKernel::transactions() const noexcept
{
    return transactions_;
}

AppKernelState AppKernel::state() const noexcept
{
    return state_;
}

} // namespace lasercnc::kernel
