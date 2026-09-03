#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/runtime/command.hpp>

#include <cstddef>
#include <memory>

namespace lasercnc::kernel {
class AppKernel;
}

namespace lasercnc::messaging {
class EventBus;
}

namespace lasercnc::observability {
class IMetricsService;
class ITraceService;
}

namespace lasercnc::persistence {
class PersistenceService;
}

namespace lasercnc::runtime {

class CapabilityService;
class CommandRegistry;
class ExecutionServices;
class TransactionManager;
class TaskRuntime;

class CommandRuntime final {
public:
    CommandRuntime(
        CommandRegistry& registry,
        TransactionManager& transactions,
        CapabilityService& capabilities,
        messaging::EventBus& events,
        ExecutionServices& executionServices,
        TaskRuntime& tasks,
        persistence::PersistenceService& persistence,
        observability::ITraceService& traces,
        observability::IMetricsService& metrics,
        std::size_t idempotencyCapacity = 1024U);
    ~CommandRuntime();

    CommandRuntime(const CommandRuntime&) = delete;
    CommandRuntime& operator=(const CommandRuntime&) = delete;

    [[nodiscard]] foundation::Result<CommandResponse> execute(const CommandRequest& request);
    [[nodiscard]] std::size_t activeExecutionCount() const noexcept;
    [[nodiscard]] std::size_t idempotencyRecordCount() const;
    [[nodiscard]] bool accepting() const noexcept;

private:
    friend class kernel::AppKernel;

    class Impl;

    void start() noexcept;
    void stop() noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace lasercnc::runtime
