#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/runtime/scheduler.hpp>
#include <lasercnc/runtime/execution_services.hpp>
#include <lasercnc/runtime/task_registry.hpp>
#include <lasercnc/state/document_store.hpp>

#include <chrono>
#include <atomic>

namespace lasercnc::runtime {

class TaskRuntime final {
public:
    TaskRuntime(
        TaskRegistry& registry,
        Scheduler& scheduler,
        ExecutionServices& executionServices,
        const state::DocumentStore& documents);

    void start() noexcept;
    void stop() noexcept;
    [[nodiscard]] foundation::Result<void> submit(TaskRequest request);
    [[nodiscard]] foundation::Result<void> cancel(const kernel::TaskId& taskId);
    [[nodiscard]] foundation::Result<TaskSnapshot> snapshot(const kernel::TaskId& taskId) const;
    [[nodiscard]] foundation::Result<TaskSnapshot> wait(
        const kernel::TaskId& taskId,
        std::chrono::milliseconds timeout) const;
    [[nodiscard]] std::size_t activeExecutionCount() const;

private:
    TaskRegistry& registry_;
    Scheduler& scheduler_;
    ExecutionServices& executionServices_;
    const state::DocumentStore& documents_;
    std::atomic_bool accepting_{false};
};

} // namespace lasercnc::runtime
