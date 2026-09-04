#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/runtime/scheduler.hpp>
#include <lasercnc/runtime/execution_services.hpp>
#include <lasercnc/runtime/task_registry.hpp>
#include <lasercnc/runtime/transaction.hpp>
#include <lasercnc/state/document_store.hpp>

#include <chrono>
#include <atomic>

namespace lasercnc::persistence {
class PersistenceService;
}

namespace lasercnc::runtime {

class CommandRuntime;
class DocumentRuntime;

class TaskRuntime final {
public:
    // Standalone lifetime owner serializes start/stop against complete submit (including activation).
    // stop closes admission only; keep this runtime and borrowed dependencies alive through executor drain.
    // 中文翻译：独立寿命所有者串行协调 start/stop 与完整 submit（含激活）；stop 只关闭准入。
    // 中文翻译：执行器排空前必须保留本运行时与借用依赖，不能并发销毁仍在调用的对象。
    TaskRuntime(
        TaskRegistry& registry,
        Scheduler& scheduler,
        ExecutionServices& executionServices,
        const state::DocumentStore& documents,
        DocumentRuntime* documentRuntime = nullptr);
    TaskRuntime(
        TaskRegistry& registry,
        Scheduler& scheduler,
        ExecutionServices& executionServices,
        const state::DocumentStore& documents,
        persistence::PersistenceService& persistence,
        DocumentRuntime* documentRuntime = nullptr);

    void start() noexcept;
    void stop() noexcept;
    [[nodiscard]] foundation::Result<void> submit(TaskRequest request);
    [[nodiscard]] foundation::Result<void> cancel(const kernel::TaskId& taskId);
    [[nodiscard]] foundation::Result<TaskSnapshot> snapshot(const kernel::TaskId& taskId) const;
    [[nodiscard]] foundation::Result<TaskSnapshot> wait(
        const kernel::TaskId& taskId,
        std::chrono::milliseconds timeout) const;
    [[nodiscard]] std::size_t activeExecutionCount() const;
    [[nodiscard]] std::size_t activeExecutionCount(
        const kernel::DocumentId& documentId) const;
    [[nodiscard]] std::size_t activeExecutionCount(
        const kernel::ProjectId& projectId) const;

private:
    friend class CommandRuntime;
    friend class kernel::AppKernel;

    [[nodiscard]] foundation::Result<void> submit(
        TaskRequest request,
        std::optional<TransactionIdempotency> commandIdempotency);

    TaskRegistry& registry_;
    Scheduler& scheduler_;
    ExecutionServices& executionServices_;
    const state::DocumentStore& documents_;
    persistence::PersistenceService* persistence_{nullptr};
    DocumentRuntime* documentRuntime_{nullptr};
    std::atomic_bool accepting_{false};
};

} // namespace lasercnc::runtime
