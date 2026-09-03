#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/kernel/module_runtime.hpp>
#include <lasercnc/kernel/service_registry.hpp>
#include <lasercnc/runtime/transaction_manager.hpp>
#include <lasercnc/messaging/event_bus.hpp>
#include <lasercnc/observability/diagnostics_service.hpp>
#include <lasercnc/observability/metrics_service.hpp>
#include <lasercnc/observability/trace_service.hpp>
#include <lasercnc/persistence/persistence_service.hpp>
#include <lasercnc/runtime/capability_service.hpp>
#include <lasercnc/runtime/command_registry.hpp>
#include <lasercnc/runtime/command_runtime.hpp>
#include <lasercnc/runtime/execution_services.hpp>
#include <lasercnc/runtime/effect_executor.hpp>
#include <lasercnc/runtime/effect_guard.hpp>
#include <lasercnc/runtime/query_registry.hpp>
#include <lasercnc/runtime/query_runtime.hpp>
#include <lasercnc/runtime/resource_manager.hpp>
#include <lasercnc/runtime/scheduler.hpp>
#include <lasercnc/runtime/script_registry.hpp>
#include <lasercnc/runtime/script_runtime.hpp>
#include <lasercnc/runtime/task_registry.hpp>
#include <lasercnc/runtime/task_runtime.hpp>
#include <lasercnc/runtime/workflow_registry.hpp>
#include <lasercnc/runtime/workflow_runtime.hpp>
#include <lasercnc/state/document_store.hpp>

#include <chrono>
#include <memory>

namespace lasercnc::kernel {

enum class AppKernelState {
    Configuring,
    Starting,
    Ready,
    Stopping,
    Stopped,
    Failed
};

class AppKernel final {
public:
    AppKernel();
    ~AppKernel();

    AppKernel(const AppKernel&) = delete;
    AppKernel& operator=(const AppKernel&) = delete;

    [[nodiscard]] foundation::Result<void> addModule(std::unique_ptr<IModule> module);
    [[nodiscard]] foundation::Result<void> configureTaskExecutor(
        std::unique_ptr<platform::ITaskExecutor> executor);
    [[nodiscard]] foundation::Result<void> bootstrap();
    [[nodiscard]] foundation::Result<void> shutdown(
        std::chrono::milliseconds taskTimeout = std::chrono::seconds(5));

    [[nodiscard]] ServiceRegistry& services() noexcept;
    [[nodiscard]] const ServiceRegistry& services() const noexcept;
    [[nodiscard]] ModuleRuntime& modules() noexcept;
    [[nodiscard]] const ModuleRuntime& modules() const noexcept;
    [[nodiscard]] foundation::Result<void> addDocument(
        ProjectId projectId,
        DocumentId documentId);
    [[nodiscard]] const state::DocumentStore& documents() const noexcept;
    [[nodiscard]] runtime::ExecutionServices& executionServices() noexcept;
    [[nodiscard]] const runtime::ExecutionServices& executionServices() const noexcept;
    [[nodiscard]] runtime::CapabilityService& capabilities() noexcept;
    [[nodiscard]] const runtime::CapabilityService& capabilities() const noexcept;
    [[nodiscard]] messaging::EventBus& events() noexcept;
    [[nodiscard]] const messaging::EventBus& events() const noexcept;
    [[nodiscard]] runtime::CommandRegistry& commandRegistry() noexcept;
    [[nodiscard]] const runtime::CommandRegistry& commandRegistry() const noexcept;
    [[nodiscard]] runtime::EffectGuardRegistry& effectGuards() noexcept;
    [[nodiscard]] const runtime::EffectGuardRegistry& effectGuards() const noexcept;
    [[nodiscard]] runtime::QueryRegistry& queryRegistry() noexcept;
    [[nodiscard]] const runtime::QueryRegistry& queryRegistry() const noexcept;
    [[nodiscard]] runtime::WorkflowRegistry& workflowRegistry() noexcept;
    [[nodiscard]] const runtime::WorkflowRegistry& workflowRegistry() const noexcept;
    [[nodiscard]] runtime::ScriptRegistry& scriptRegistry() noexcept;
    [[nodiscard]] const runtime::ScriptRegistry& scriptRegistry() const noexcept;
    [[nodiscard]] runtime::CommandRuntime& commands() noexcept;
    [[nodiscard]] const runtime::CommandRuntime& commands() const noexcept;
    [[nodiscard]] runtime::QueryRuntime& queries() noexcept;
    [[nodiscard]] const runtime::QueryRuntime& queries() const noexcept;
    [[nodiscard]] runtime::TaskRegistry& taskRegistry() noexcept;
    [[nodiscard]] const runtime::TaskRegistry& taskRegistry() const noexcept;
    [[nodiscard]] runtime::ResourceManager& resources() noexcept;
    [[nodiscard]] const runtime::ResourceManager& resources() const noexcept;
    [[nodiscard]] runtime::Scheduler& scheduler() noexcept;
    [[nodiscard]] const runtime::Scheduler& scheduler() const noexcept;
    [[nodiscard]] runtime::TaskRuntime& tasks() noexcept;
    [[nodiscard]] const runtime::TaskRuntime& tasks() const noexcept;
    [[nodiscard]] runtime::WorkflowRuntime& workflows() noexcept;
    [[nodiscard]] const runtime::WorkflowRuntime& workflows() const noexcept;
    [[nodiscard]] runtime::ScriptRuntime& scripts() noexcept;
    [[nodiscard]] const runtime::ScriptRuntime& scripts() const noexcept;
    [[nodiscard]] observability::LocalTraceService& traces() noexcept;
    [[nodiscard]] const observability::LocalTraceService& traces() const noexcept;
    [[nodiscard]] observability::LocalMetricsService& metrics() noexcept;
    [[nodiscard]] const observability::LocalMetricsService& metrics() const noexcept;
    [[nodiscard]] observability::DiagnosticsService& diagnostics() noexcept;
    [[nodiscard]] const observability::DiagnosticsService& diagnostics() const noexcept;
    [[nodiscard]] persistence::PersistenceService& persistence() noexcept;
    [[nodiscard]] const persistence::PersistenceService& persistence() const noexcept;
    [[nodiscard]] AppKernelState state() const noexcept;

private:
    ServiceRegistry services_;
    ModuleRuntime modules_;
    state::DocumentStore documents_;
    persistence::PersistenceService persistence_;
    runtime::TransactionManager transactions_;
    runtime::ExecutionServices executionServices_;
    runtime::CapabilityService capabilities_;
    messaging::EventBus events_;
    observability::LocalTraceService traces_;
    observability::LocalMetricsService metrics_;
    observability::DiagnosticsService diagnostics_;
    runtime::CommandRegistry commandRegistry_;
    runtime::EffectGuardRegistry effectGuards_;
    runtime::QueryRegistry queryRegistry_;
    runtime::WorkflowRegistry workflowRegistry_;
    runtime::ScriptRegistry scriptRegistry_;
    runtime::TaskRegistry taskRegistry_;
    runtime::ResourceManager resources_;
    runtime::EffectExecutor effects_;
    std::unique_ptr<platform::ITaskExecutor> taskExecutor_;
    runtime::Scheduler scheduler_;
    runtime::TaskRuntime tasks_;
    runtime::CommandRuntime commands_;
    runtime::QueryRuntime queries_;
    runtime::WorkflowRuntime workflows_;
    runtime::ScriptRuntime scripts_;
    AppKernelState state_{AppKernelState::Configuring};
};

} // namespace lasercnc::kernel
