#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/version.hpp>
#include <lasercnc/kernel/identifiers.hpp>
#include <string>
#include <vector>

namespace lasercnc::kernel {

class AppKernel;
class ModuleRegistrar;

struct ModuleDependency final {
    ModuleId id;
    foundation::Version minimumVersion;
};

struct ModuleDescriptor final {
    ModuleId id;
    std::string displayName;
    foundation::Version version;
    std::vector<ModuleDependency> dependencies;
    std::vector<ServiceId> requiredServices;
    std::vector<ServiceId> providedServices;
    std::vector<CommandName> commands;
    std::vector<QueryName> queries;
    std::vector<TaskName> tasks;
    std::vector<WorkflowName> workflows;
    std::vector<ScriptName> scripts;
    std::vector<EventName> events;
    std::vector<CapabilityId> capabilities;
};

class IModule {
public:
    virtual ~IModule() = default;

    [[nodiscard]] virtual const ModuleDescriptor& descriptor() const noexcept = 0;
    [[nodiscard]] virtual foundation::Result<void> registerComponents(
        ModuleRegistrar& registrar);
    [[nodiscard]] virtual foundation::Result<void> initialize(AppKernel& kernel);
    [[nodiscard]] virtual foundation::Result<void> start(AppKernel& kernel);
    [[nodiscard]] virtual foundation::Result<void> stop(AppKernel& kernel);
};

} // namespace lasercnc::kernel
