#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/kernel/module_runtime.hpp>
#include <lasercnc/kernel/service_registry.hpp>

#include <memory>

namespace lasercnc::kernel {

enum class AppKernelState {
    Configuring,
    Starting,
    Ready,
    Stopping,
    Stopped,
    Failed
};

class AppKernel final {
public:
    AppKernel();
    ~AppKernel();

    AppKernel(const AppKernel&) = delete;
    AppKernel& operator=(const AppKernel&) = delete;

    [[nodiscard]] foundation::Result<void> addModule(std::unique_ptr<IModule> module);
    [[nodiscard]] foundation::Result<void> bootstrap();
    [[nodiscard]] foundation::Result<void> shutdown();

    [[nodiscard]] ServiceRegistry& services() noexcept;
    [[nodiscard]] const ServiceRegistry& services() const noexcept;
    [[nodiscard]] ModuleRuntime& modules() noexcept;
    [[nodiscard]] const ModuleRuntime& modules() const noexcept;
    [[nodiscard]] AppKernelState state() const noexcept;

private:
    ServiceRegistry services_;
    ModuleRuntime modules_;
    AppKernelState state_{AppKernelState::Configuring};
};

} // namespace lasercnc::kernel
