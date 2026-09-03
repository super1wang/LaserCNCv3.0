#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/runtime/script.hpp>

#include <cstddef>
#include <memory>

namespace lasercnc::kernel {
class AppKernel;
}

namespace lasercnc::observability {
class IMetricsService;
class ITraceService;
}

namespace lasercnc::runtime {

class CommandRuntime;
class ExecutionServices;
class QueryRuntime;
class ScriptRegistry;
class TaskRuntime;
class WorkflowRuntime;

class ScriptRuntime final {
public:
    ScriptRuntime(
        ScriptRegistry& registry,
        CommandRuntime& commands,
        QueryRuntime& queries,
        WorkflowRuntime& workflows,
        TaskRuntime& tasks,
        ExecutionServices& executionServices,
        observability::ITraceService& traces,
        observability::IMetricsService& metrics,
        std::size_t executionNodeLimit = 10000U,
        std::size_t includeDepthLimit = 32U);
    ~ScriptRuntime();

    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    [[nodiscard]] foundation::Result<ScriptSnapshot> startScript(ScriptRequest request);
    [[nodiscard]] foundation::Result<ScriptSnapshot> advance(
        const kernel::ScriptExecutionId& executionId);
    [[nodiscard]] foundation::Result<ScriptSnapshot> cancel(
        const kernel::ScriptExecutionId& executionId);
    [[nodiscard]] foundation::Result<ScriptSnapshot> snapshot(
        const kernel::ScriptExecutionId& executionId) const;
    [[nodiscard]] std::size_t activeExecutionCount() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;

private:
    friend class kernel::AppKernel;

    class Impl;
    void start() noexcept;
    void stop() noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace lasercnc::runtime
