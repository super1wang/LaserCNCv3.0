#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/observability/metrics_service.hpp>
#include <lasercnc/observability/trace_service.hpp>
#include <lasercnc/platform/task_executor.hpp>
#include <lasercnc/runtime/resource_manager.hpp>
#include <lasercnc/runtime/task.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace lasercnc::persistence {
class PersistenceService;
}

namespace lasercnc::runtime {

class TaskRuntime;

class Scheduler final {
public:
    Scheduler(
        ResourceManager& resources,
        observability::ITraceService& traces,
        observability::IMetricsService& metrics);
    Scheduler(
        ResourceManager& resources,
        persistence::PersistenceService& persistence,
        observability::ITraceService& traces,
        observability::IMetricsService& metrics);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    [[nodiscard]] foundation::Result<void> configureExecutor(
        platform::ITaskExecutor& executor);
    [[nodiscard]] foundation::Result<void> start();
    [[nodiscard]] foundation::Result<void> requestCancel(const kernel::TaskId& taskId);
    [[nodiscard]] foundation::Result<TaskSnapshot> snapshot(const kernel::TaskId& taskId) const;
    [[nodiscard]] foundation::Result<TaskSnapshot> wait(
        const kernel::TaskId& taskId,
        std::chrono::milliseconds timeout) const;
    // Seals admission permanently; success requires terminal publication and executor acknowledgement.
    // Timeout bounds the scheduler drain wait, not arbitrary executor/callback implementations.
    // 中文翻译：永久封闭准入；成功须终态发布及执行器确认。超时约束调度器排空等待，不强制中断外部实现。
    [[nodiscard]] foundation::Result<void> shutdown(std::chrono::milliseconds timeout);
    // Includes terminal tasks until observation and persistence completion are published.
    // 中文翻译：终态任务在观察和持久化完成发布之前仍计入活动数量。
    [[nodiscard]] std::size_t activeTaskCount() const;
    [[nodiscard]] std::size_t activeTaskCount(
        const kernel::DocumentId& documentId) const;
    [[nodiscard]] std::size_t activeTaskCount(
        const kernel::ProjectId& projectId) const;
    [[nodiscard]] std::vector<foundation::Error> persistenceFailures() const;

private:
    struct Core;
    struct Outcome;
    std::shared_ptr<Core> core_;

    static void pump(const std::shared_ptr<Core>& core);
    static void finish(
        const std::shared_ptr<Core>& core,
        const kernel::TaskId& taskId,
        foundation::Result<void> executionResult,
        const std::shared_ptr<Outcome>& outcome);
    static void persistTerminal(
        const std::shared_ptr<Core>& core,
        const TaskSnapshot& snapshot) noexcept;

    [[nodiscard]] foundation::Result<void> schedule(
        TaskDescriptor descriptor,
        std::shared_ptr<ITaskHandler> handler,
        TaskRequest request,
        std::optional<state::Document> document,
        std::function<bool()> sourceIsStale,
        bool activateImmediately);
    [[nodiscard]] foundation::Result<void> activate(const kernel::TaskId& taskId);
    void discardPrepared(const kernel::TaskId& taskId) noexcept;

    friend class TaskRuntime;
};

} // namespace lasercnc::runtime
