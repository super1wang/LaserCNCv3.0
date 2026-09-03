#include <lasercnc/kernel/module_registrar.hpp>

#include <lasercnc/foundation/value.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace lasercnc::kernel {
namespace {

foundation::Value contributionDetails(
    const ModuleDescriptor& descriptor,
    std::string_view kind,
    std::string_view identity)
{
    return foundation::Value {foundation::Value::Object {
        {"contribution", foundation::Value {std::string(identity)}},
        {"kind", foundation::Value {std::string(kind)}},
        {"moduleId", foundation::Value {std::string(descriptor.id.value())}},
    }};
}

template <typename Id>
std::vector<Id> normalized(std::vector<Id> values)
{
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

template <typename Id>
bool matches(const std::vector<Id>& declared, const std::vector<Id>& registered)
{
    return normalized(declared) == normalized(registered);
}

} // namespace

ModuleRegistrar::ModuleRegistrar(
    const ModuleDescriptor& descriptor,
    ServiceRegistry& services,
    runtime::CommandRegistry& commands,
    runtime::QueryRegistry& queries,
    runtime::TaskRegistry& tasks,
    runtime::WorkflowRegistry& workflows,
    runtime::ScriptRegistry& scripts,
    state::ObjectTypeRegistry& objectTypes) noexcept
    : descriptor_(descriptor),
      services_(services),
      commands_(commands),
      queries_(queries),
      tasks_(tasks),
      workflows_(workflows),
      scripts_(scripts),
      objectTypes_(objectTypes)
{
}

foundation::Result<void> ModuleRegistrar::admit(
    bool declared,
    std::string_view kind,
    std::string_view identity)
{
    if(declared) {
        return foundation::Result<void>::success();
    }
    return remember(foundation::makeError(
        "Kernel.ModuleContributionUndeclared",
        foundation::ErrorCategory::Validation,
        "A module attempted to register an undeclared contribution",
        contributionDetails(descriptor_, kind, identity)));
}

foundation::Result<void> ModuleRegistrar::remember(foundation::Error error)
{
    if(!firstError_.has_value()) {
        firstError_ = error;
    }
    return foundation::Result<void>::failure(std::move(error));
}

foundation::Result<void> ModuleRegistrar::registerCommand(
    runtime::CommandDescriptor descriptor,
    std::shared_ptr<runtime::ICommandHandler> handler)
{
    const runtime::CommandKey key {descriptor.name, descriptor.version};
    auto admitted = admit(
        contains(descriptor_.commands, key),
        "command",
        descriptor.name.value());
    if(!admitted) {
        return admitted;
    }
    auto registered = commands_.registerHandler(std::move(descriptor), std::move(handler));
    if(!registered) {
        return remember(std::move(registered).error());
    }
    contributions_.commands.push_back(key);
    return foundation::Result<void>::success();
}

foundation::Result<void> ModuleRegistrar::registerAsyncCommand(
    runtime::CommandDescriptor descriptor,
    std::shared_ptr<runtime::IAsyncCommandHandler> handler)
{
    const runtime::CommandKey key {descriptor.name, descriptor.version};
    auto admitted = admit(
        contains(descriptor_.commands, key),
        "command",
        descriptor.name.value());
    if(!admitted) {
        return admitted;
    }
    auto registered = commands_.registerAsyncHandler(
        std::move(descriptor), std::move(handler));
    if(!registered) {
        return remember(std::move(registered).error());
    }
    contributions_.commands.push_back(key);
    return foundation::Result<void>::success();
}

foundation::Result<void> ModuleRegistrar::registerReadOnlyCommand(
    runtime::CommandDescriptor descriptor,
    std::shared_ptr<runtime::IReadOnlyCommandHandler> handler)
{
    const runtime::CommandKey key {descriptor.name, descriptor.version};
    auto admitted = admit(
        contains(descriptor_.commands, key),
        "command",
        descriptor.name.value());
    if(!admitted) {
        return admitted;
    }
    auto registered = commands_.registerReadOnlyHandler(
        std::move(descriptor), std::move(handler));
    if(!registered) {
        return remember(std::move(registered).error());
    }
    contributions_.commands.push_back(key);
    return foundation::Result<void>::success();
}

foundation::Result<void> ModuleRegistrar::registerExternalEffectCommand(
    runtime::CommandDescriptor descriptor,
    std::shared_ptr<runtime::IExternalEffectHandler> handler)
{
    const runtime::CommandKey key {descriptor.name, descriptor.version};
    auto admitted = admit(
        contains(descriptor_.commands, key),
        "command",
        descriptor.name.value());
    if(!admitted) {
        return admitted;
    }
    auto registered = commands_.registerExternalEffectHandler(
        std::move(descriptor), std::move(handler));
    if(!registered) {
        return remember(std::move(registered).error());
    }
    contributions_.commands.push_back(key);
    return foundation::Result<void>::success();
}

foundation::Result<void> ModuleRegistrar::registerQuery(
    runtime::QueryDescriptor descriptor,
    std::shared_ptr<runtime::IQueryHandler> handler)
{
    const runtime::QueryKey key {descriptor.name, descriptor.version};
    auto admitted = admit(
        contains(descriptor_.queries, key),
        "query",
        descriptor.name.value());
    if(!admitted) {
        return admitted;
    }
    auto registered = queries_.registerHandler(std::move(descriptor), std::move(handler));
    if(!registered) {
        return remember(std::move(registered).error());
    }
    contributions_.queries.push_back(key);
    return foundation::Result<void>::success();
}

foundation::Result<void> ModuleRegistrar::registerTask(
    runtime::TaskDescriptor descriptor,
    std::shared_ptr<runtime::ITaskHandler> handler)
{
    const auto name = descriptor.name;
    auto admitted = admit(
        contains(descriptor_.tasks, name), "task", name.value());
    if(!admitted) {
        return admitted;
    }
    auto registered = tasks_.registerHandler(std::move(descriptor), std::move(handler));
    if(!registered) {
        return remember(std::move(registered).error());
    }
    contributions_.tasks.push_back(name);
    return foundation::Result<void>::success();
}

foundation::Result<void> ModuleRegistrar::registerWorkflow(
    runtime::WorkflowDefinition definition)
{
    const auto name = definition.descriptor.name;
    auto admitted = admit(
        contains(descriptor_.workflows, name), "workflow", name.value());
    if(!admitted) {
        return admitted;
    }
    auto registered = workflows_.registerDefinition(std::move(definition));
    if(!registered) {
        return remember(std::move(registered).error());
    }
    contributions_.workflows.push_back(name);
    return foundation::Result<void>::success();
}

foundation::Result<void> ModuleRegistrar::registerScript(
    runtime::ScriptDefinition definition)
{
    const auto name = definition.descriptor.name;
    auto admitted = admit(
        contains(descriptor_.scripts, name), "script", name.value());
    if(!admitted) {
        return admitted;
    }
    auto registered = scripts_.registerDefinition(std::move(definition));
    if(!registered) {
        return remember(std::move(registered).error());
    }
    contributions_.scripts.push_back(name);
    return foundation::Result<void>::success();
}

foundation::Result<void> ModuleRegistrar::registerEvent(EventName event)
{
    auto admitted = admit(
        contains(descriptor_.events, event), "event", event.value());
    if(!admitted) {
        return admitted;
    }
    if(contains(contributions_.events, event)) {
        return remember(foundation::makeError(
            "Kernel.ModuleContributionDuplicated",
            foundation::ErrorCategory::Validation,
            "A module contribution was registered more than once",
            contributionDetails(descriptor_, "event", event.value())));
    }
    contributions_.events.push_back(std::move(event));
    return foundation::Result<void>::success();
}

foundation::Result<void> ModuleRegistrar::registerCapability(CapabilityId capability)
{
    auto admitted = admit(
        contains(descriptor_.capabilities, capability),
        "capability",
        capability.value());
    if(!admitted) {
        return admitted;
    }
    if(contains(contributions_.capabilities, capability)) {
        return remember(foundation::makeError(
            "Kernel.ModuleContributionDuplicated",
            foundation::ErrorCategory::Validation,
            "A module contribution was registered more than once",
            contributionDetails(descriptor_, "capability", capability.value())));
    }
    contributions_.capabilities.push_back(std::move(capability));
    return foundation::Result<void>::success();
}

foundation::Result<void> ModuleRegistrar::registerObjectType(
    state::ObjectTypeDefinition definition)
{
    const auto type = definition.descriptor.type;
    auto admitted = admit(
        contains(descriptor_.objectTypes, type), "object-type", type.value());
    if(!admitted) {
        return admitted;
    }
    auto registered = objectTypes_.registerType(std::move(definition));
    if(!registered) {
        return remember(std::move(registered).error());
    }
    contributions_.objectTypes.push_back(type);
    return foundation::Result<void>::success();
}

foundation::Result<ModuleContributionSnapshot> ModuleRegistrar::finish()
{
    if(firstError_.has_value()) {
        return foundation::Result<ModuleContributionSnapshot>::failure(*firstError_);
    }
    const bool exact = matches(descriptor_.providedServices, contributions_.services)
        && matches(descriptor_.commands, contributions_.commands)
        && matches(descriptor_.queries, contributions_.queries)
        && matches(descriptor_.tasks, contributions_.tasks)
        && matches(descriptor_.workflows, contributions_.workflows)
        && matches(descriptor_.scripts, contributions_.scripts)
        && matches(descriptor_.events, contributions_.events)
        && matches(descriptor_.capabilities, contributions_.capabilities)
        && matches(descriptor_.objectTypes, contributions_.objectTypes);
    if(!exact) {
        return foundation::Result<ModuleContributionSnapshot>::failure(
            foundation::makeError(
                "Kernel.ModuleContributionDeclarationMismatch",
                foundation::ErrorCategory::Validation,
                "Registered contributions do not match the module descriptor",
                contributionDetails(descriptor_, "module", descriptor_.id.value())));
    }
    return foundation::Result<ModuleContributionSnapshot>::success(
        std::move(contributions_));
}

} // namespace lasercnc::kernel
