#pragma once

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/foundation/result.hpp>
#include <lasercnc/kernel/module.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace lasercnc::kernel {

class AppKernel;

enum class ModuleState {
    Discovered,
    Registered,
    Initialized,
    Started,
    Ready,
    Stopping,
    Stopped,
    Failed
};

enum class ModuleRuntimeState {
    Configuring,
    Starting,
    Ready,
    Stopping,
    Stopped,
    Failed
};

struct ModuleSnapshot final {
    ModuleId id;
    foundation::Version version;
    ModuleState state{ModuleState::Discovered};
    std::optional<foundation::Error> lastError;
};

class ModuleRuntime final {
public:
    explicit ModuleRuntime(ServiceRegistry& services);

    ModuleRuntime(const ModuleRuntime&) = delete;
    ModuleRuntime& operator=(const ModuleRuntime&) = delete;

    [[nodiscard]] foundation::Result<void> addModule(std::unique_ptr<IModule> module);
    [[nodiscard]] foundation::Result<void> bootstrap(AppKernel& kernel);
    [[nodiscard]] foundation::Result<void> shutdown(AppKernel& kernel);

    [[nodiscard]] ModuleRuntimeState state() const noexcept;
    [[nodiscard]] std::vector<ModuleSnapshot> snapshot() const;

private:
    struct Record final {
        std::unique_ptr<IModule> module;
        ModuleDescriptor descriptor;
        ModuleState state{ModuleState::Discovered};
        std::optional<foundation::Error> lastError;
        std::vector<ServiceId> registeredServices;
    };

    [[nodiscard]] foundation::Result<std::vector<std::size_t>> buildStartupOrder() const;
    [[nodiscard]] foundation::Result<void> validateServiceDeclarations() const;
    [[nodiscard]] foundation::Result<void> validateRegisteredServices(const Record& record) const;
    [[nodiscard]] foundation::Result<void> validateRequiredServices(const Record& record) const;
    void captureRegistrationDelta(Record& record, const std::vector<ServiceId>& before);
    [[nodiscard]] std::optional<foundation::Error> rollback(
        AppKernel& kernel,
        std::optional<std::size_t> failedIndex);

    ServiceRegistry& services_;
    std::vector<Record> records_;
    std::vector<std::size_t> startupOrder_;
    ModuleRuntimeState state_{ModuleRuntimeState::Configuring};
};

} // namespace lasercnc::kernel
