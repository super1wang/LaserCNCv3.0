#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/kernel/module_runtime.hpp>
#include <lasercnc/kernel/service_registry.hpp>
#include <lasercnc/runtime/transaction_manager.hpp>
#include <lasercnc/messaging/event_bus.hpp>
#include <lasercnc/runtime/capability_service.hpp>
#include <lasercnc/runtime/command_registry.hpp>
#include <lasercnc/runtime/command_runtime.hpp>
#include <lasercnc/runtime/execution_services.hpp>
#include <lasercnc/runtime/query_registry.hpp>
#include <lasercnc/runtime/query_runtime.hpp>
#include <lasercnc/state/document_store.hpp>

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
    [[nodiscard]] foundation::Result<void> bootstrap();
    [[nodiscard]] foundation::Result<void> shutdown();

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
    [[nodiscard]] runtime::QueryRegistry& queryRegistry() noexcept;
    [[nodiscard]] const runtime::QueryRegistry& queryRegistry() const noexcept;
    [[nodiscard]] runtime::CommandRuntime& commands() noexcept;
    [[nodiscard]] const runtime::CommandRuntime& commands() const noexcept;
    [[nodiscard]] runtime::QueryRuntime& queries() noexcept;
    [[nodiscard]] const runtime::QueryRuntime& queries() const noexcept;
    [[nodiscard]] AppKernelState state() const noexcept;

private:
    ServiceRegistry services_;
    ModuleRuntime modules_;
    state::DocumentStore documents_;
    runtime::TransactionManager transactions_;
    runtime::ExecutionServices executionServices_;
    runtime::CapabilityService capabilities_;
    messaging::EventBus events_;
    runtime::CommandRegistry commandRegistry_;
    runtime::QueryRegistry queryRegistry_;
    runtime::CommandRuntime commands_;
    runtime::QueryRuntime queries_;
    AppKernelState state_{AppKernelState::Configuring};
};

} // namespace lasercnc::kernel
