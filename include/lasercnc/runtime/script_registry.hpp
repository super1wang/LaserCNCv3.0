#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/runtime/script.hpp>

#include <map>
#include <shared_mutex>
#include <vector>

namespace lasercnc::kernel {
class AppKernel;
}

namespace lasercnc::runtime {

class CommandRegistry;
class QueryRegistry;
class ScriptRuntime;
class WorkflowRegistry;

class ScriptRegistry final {
public:
    ScriptRegistry(
        const CommandRegistry& commands,
        const QueryRegistry& queries,
        const WorkflowRegistry& workflows) noexcept;

    [[nodiscard]] foundation::Result<void> registerDefinition(ScriptDefinition definition);
    [[nodiscard]] foundation::Result<ScriptDescriptor> descriptor(
        const kernel::ScriptName& name) const;
    [[nodiscard]] std::vector<ScriptDescriptor> descriptors() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool frozen() const;

private:
    friend class ScriptRuntime;
    friend class kernel::AppKernel;

    [[nodiscard]] foundation::Result<ScriptDefinition> resolve(
        const kernel::ScriptName& name) const;
    [[nodiscard]] foundation::Result<void> validateAndFreeze();

    const CommandRegistry& commands_;
    const QueryRegistry& queries_;
    const WorkflowRegistry& workflows_;
    mutable std::shared_mutex mutex_;
    std::map<kernel::ScriptName, ScriptDefinition> definitions_;
    bool frozen_{false};
};

} // namespace lasercnc::runtime
