#pragma once

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/foundation/result.hpp>
#include <lasercnc/kernel/module.hpp>
#include <lasercnc/kernel/module_registrar.hpp>

#include <cstddef>
#include <functional>
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
    ModuleRuntime(
        ServiceRegistry& services,
        runtime::CommandRegistry& commands,
        runtime::QueryRegistry& queries,
        runtime::TaskRegistry& tasks,
        runtime::WorkflowRegistry& workflows,
        runtime::ScriptRegistry& scripts,
        state::ObjectTypeRegistry& objectTypes) noexcept;

    ModuleRuntime(const ModuleRuntime&) = delete;
    ModuleRuntime& operator=(const ModuleRuntime&) = delete;

    [[nodiscard]] foundation::Result<void> addModule(std::unique_ptr<IModule> module);
    [[nodiscard]] foundation::Result<void> bootstrap(AppKernel& kernel);
    [[nodiscard]] foundation::Result<void> shutdown(AppKernel& kernel);

    [[nodiscard]] ModuleRuntimeState state() const noexcept;
    [[nodiscard]] std::vector<ModuleSnapshot> snapshot() const;

private:
    friend class AppKernel;
    [[nodiscard]] foundation::Result<void> bootstrap(
        AppKernel& kernel, const std::function<foundation::Result<void>()>& beforeInitialize);

    struct Record final {
        std::unique_ptr<IModule> module;
        ModuleDescriptor descriptor;
        ModuleState state{ModuleState::Discovered};
        std::optional<foundation::Error> lastError;
        ModuleContributionSnapshot contributions;
    };

    [[nodiscard]] foundation::Result<std::vector<std::size_t>> buildStartupOrder() const;
    [[nodiscard]] foundation::Result<void> validateContributionDeclarations() const;
    [[nodiscard]] foundation::Result<void> validateRequiredServices(const Record& record) const;
    void removeContributions(Record& record);
    [[nodiscard]] std::optional<foundation::Error> rollback(
        AppKernel& kernel,
        std::optional<std::size_t> failedIndex);

    ServiceRegistry& services_;
    runtime::CommandRegistry& commands_;
    runtime::QueryRegistry& queries_;
    runtime::TaskRegistry& tasks_;
    runtime::WorkflowRegistry& workflows_;
    runtime::ScriptRegistry& scripts_;
    state::ObjectTypeRegistry& objectTypes_;
    std::vector<Record> records_;
    std::vector<std::size_t> startupOrder_;
    ModuleRuntimeState state_{ModuleRuntimeState::Configuring};
};

} // namespace lasercnc::kernel
