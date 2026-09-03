#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/runtime/workflow.hpp>

#include <cstddef>
#include <memory>

namespace lasercnc::kernel {
class AppKernel;
}

namespace lasercnc::persistence {
class PersistenceService;
}

namespace lasercnc::runtime {

class CommandRuntime;
class ExecutionServices;
class QueryRuntime;
class TaskRuntime;
class WorkflowRegistry;

class WorkflowRuntime final {
public:
    WorkflowRuntime(
        WorkflowRegistry& registry,
        CommandRuntime& commands,
        QueryRuntime& queries,
        TaskRuntime& tasks,
        ExecutionServices& executionServices,
        persistence::PersistenceService& persistence);
    ~WorkflowRuntime();

    WorkflowRuntime(const WorkflowRuntime&) = delete;
    WorkflowRuntime& operator=(const WorkflowRuntime&) = delete;

    [[nodiscard]] foundation::Result<WorkflowSnapshot> startWorkflow(
        WorkflowRequest request);
    [[nodiscard]] foundation::Result<WorkflowSnapshot> advance(
        const kernel::WorkflowId& workflowId);
    [[nodiscard]] foundation::Result<WorkflowSnapshot> cancel(
        const kernel::WorkflowId& workflowId);
    [[nodiscard]] foundation::Result<WorkflowSnapshot> snapshot(
        const kernel::WorkflowId& workflowId) const;
    [[nodiscard]] std::size_t activeExecutionCount() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;

private:
    friend class kernel::AppKernel;

    class Impl;

    void start() noexcept;
    void stop() noexcept;
    [[nodiscard]] foundation::Result<void> restore();

    std::unique_ptr<Impl> impl_;
};

} // namespace lasercnc::runtime
