#pragma once

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/foundation/result.hpp>
#include <lasercnc/kernel/module.hpp>
#include <lasercnc/kernel/service_registry.hpp>
#include <lasercnc/runtime/command_registry.hpp>
#include <lasercnc/runtime/query_registry.hpp>
#include <lasercnc/runtime/script_registry.hpp>
#include <lasercnc/runtime/task_registry.hpp>
#include <lasercnc/runtime/workflow_registry.hpp>
#include <lasercnc/state/object_type_registry.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lasercnc::kernel {

class ModuleRuntime;

struct ModuleContributionSnapshot final {
    std::vector<ServiceId> services;
    std::vector<runtime::CommandKey> commands;
    std::vector<runtime::QueryKey> queries;
    std::vector<TaskName> tasks;
    std::vector<WorkflowName> workflows;
    std::vector<ScriptName> scripts;
    std::vector<EventName> events;
    std::vector<CapabilityId> capabilities;
    std::vector<ObjectTypeId> objectTypes;
};

class ModuleRegistrar final {
public:
    ModuleRegistrar(const ModuleRegistrar&) = delete;
    ModuleRegistrar& operator=(const ModuleRegistrar&) = delete;

    template <typename Service>
    [[nodiscard]] foundation::Result<void> registerService(
        ServiceId id,
        std::shared_ptr<Service> service)
    {
        auto admitted = admit(
            contains(descriptor_.providedServices, id),
            "service",
            id.value());
        if(!admitted) {
            return admitted;
        }
        auto registered = services_.registerService(id, std::move(service));
        if(!registered) {
            return remember(std::move(registered).error());
        }
        contributions_.services.push_back(std::move(id));
        return foundation::Result<void>::success();
    }

    [[nodiscard]] foundation::Result<void> registerLifecycleCommand(runtime::CommandDescriptor descriptor);
    [[nodiscard]] foundation::Result<void> registerCommand(
        runtime::CommandDescriptor descriptor,
        std::shared_ptr<runtime::ICommandHandler> handler);
    [[nodiscard]] foundation::Result<void> registerAsyncCommand(
        runtime::CommandDescriptor descriptor,
        std::shared_ptr<runtime::IAsyncCommandHandler> handler);
    [[nodiscard]] foundation::Result<void> registerReadOnlyCommand(
        runtime::CommandDescriptor descriptor,
        std::shared_ptr<runtime::IReadOnlyCommandHandler> handler);
    [[nodiscard]] foundation::Result<void> registerExternalEffectCommand(
        runtime::CommandDescriptor descriptor,
        std::shared_ptr<runtime::IExternalEffectHandler> handler);
    [[nodiscard]] foundation::Result<void> registerQuery(
        runtime::QueryDescriptor descriptor,
        std::shared_ptr<runtime::IQueryHandler> handler);
    [[nodiscard]] foundation::Result<void> registerTask(
        runtime::TaskDescriptor descriptor,
        std::shared_ptr<runtime::ITaskHandler> handler);
    [[nodiscard]] foundation::Result<void> registerWorkflow(
        runtime::WorkflowDefinition definition);
    [[nodiscard]] foundation::Result<void> registerScript(
        runtime::ScriptDefinition definition);
    [[nodiscard]] foundation::Result<void> registerEvent(EventName event);
    [[nodiscard]] foundation::Result<void> registerCapability(CapabilityId capability);
    [[nodiscard]] foundation::Result<void> registerObjectType(
        state::ObjectTypeDefinition definition);

private:
    friend class ModuleRuntime;

    ModuleRegistrar(
        const ModuleDescriptor& descriptor,
        ServiceRegistry& services,
        runtime::CommandRegistry& commands,
        runtime::QueryRegistry& queries,
        runtime::TaskRegistry& tasks,
        runtime::WorkflowRegistry& workflows,
        runtime::ScriptRegistry& scripts,
        state::ObjectTypeRegistry& objectTypes) noexcept;

    template <typename Id>
    static bool contains(const std::vector<Id>& values, const Id& value)
    {
        return std::find(values.begin(), values.end(), value) != values.end();
    }

    [[nodiscard]] foundation::Result<void> admit(
        bool declared,
        std::string_view kind,
        std::string_view identity);
    [[nodiscard]] foundation::Result<void> remember(foundation::Error error);
    [[nodiscard]] foundation::Result<ModuleContributionSnapshot> finish();

    const ModuleDescriptor& descriptor_;
    ServiceRegistry& services_;
    runtime::CommandRegistry& commands_;
    runtime::QueryRegistry& queries_;
    runtime::TaskRegistry& tasks_;
    runtime::WorkflowRegistry& workflows_;
    runtime::ScriptRegistry& scripts_;
    state::ObjectTypeRegistry& objectTypes_;
    ModuleContributionSnapshot contributions_;
    std::optional<foundation::Error> firstError_;
};

} // namespace lasercnc::kernel
