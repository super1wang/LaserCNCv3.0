#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/runtime/workflow.hpp>

#include <map>
#include <shared_mutex>
#include <vector>

namespace lasercnc::kernel {
class AppKernel;
}

namespace lasercnc::runtime {

class CommandRegistry;
class QueryRegistry;
class WorkflowRuntime;

class WorkflowRegistry final {
public:
    WorkflowRegistry(const CommandRegistry& commands, const QueryRegistry& queries) noexcept;

    [[nodiscard]] foundation::Result<void> registerDefinition(WorkflowDefinition definition);
    [[nodiscard]] foundation::Result<WorkflowDescriptor> descriptor(
        const kernel::WorkflowName& name) const;
    [[nodiscard]] std::vector<WorkflowDescriptor> descriptors() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool frozen() const;

private:
    friend class WorkflowRuntime;
    friend class kernel::AppKernel;

    [[nodiscard]] foundation::Result<WorkflowDefinition> resolve(
        const kernel::WorkflowName& name) const;
    [[nodiscard]] foundation::Result<void> validateAndFreeze();

    const CommandRegistry& commands_;
    const QueryRegistry& queries_;
    mutable std::shared_mutex mutex_;
    std::map<kernel::WorkflowName, WorkflowDefinition> definitions_;
    bool frozen_{false};
};

} // namespace lasercnc::runtime
