#include <lasercnc/kernel/app_kernel.hpp>

#include <lasercnc/foundation/error.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace lasercnc::kernel {
namespace {

class PersistenceDiagnosticExporter final
    : public observability::IDiagnosticExporter {
public:
    explicit PersistenceDiagnosticExporter(
        persistence::PersistenceService& persistence) noexcept
        : persistence_(&persistence)
    {
    }

    foundation::Result<void> exportReport(
        const observability::DiagnosticReport& report) override
    {
        if(!persistence_->configured()) {
            return foundation::Result<void>::success();
        }
        return persistence_->recordDiagnostic(report);
    }

private:
    persistence::PersistenceService* persistence_;
};

} // namespace

AppKernel::AppKernel()
    : modules_(services_),
      documentRuntime_(documents_, persistence_),
      transactions_(documents_, &persistence_, &documentRuntime_),
      workflowRegistry_(commandRegistry_, queryRegistry_),
      scriptRegistry_(commandRegistry_, queryRegistry_, workflowRegistry_),
      effects_(effectGuards_, resources_, documents_, persistence_),
      scheduler_(resources_, persistence_, traces_, metrics_),
      tasks_(
          taskRegistry_,
          scheduler_,
          executionServices_,
          documents_,
          persistence_,
          &documentRuntime_),
      commands_(
          commandRegistry_,
          documents_,
          effects_,
          transactions_,
          capabilities_,
          events_,
          executionServices_,
          tasks_,
          persistence_,
          traces_,
          metrics_,
          1024U,
          &documentRuntime_),
      queries_(
          queryRegistry_,
          documents_,
          capabilities_,
          executionServices_,
          traces_,
          metrics_,
          &documentRuntime_),
      workflows_(
          workflowRegistry_,
          commands_,
          queries_,
          tasks_,
          executionServices_,
          persistence_,
          traces_,
          metrics_,
          &documentRuntime_),
      scripts_(
          scriptRegistry_,
          commands_,
          queries_,
          workflows_,
          tasks_,
          executionServices_,
          traces_,
          metrics_,
          10000U,
          32U,
          &documentRuntime_)
{
    documentRuntime_.configureCloseBlockers(
        runtime::DocumentRuntime::CloseBlockers {
            [this](const DocumentId& documentId) {
                return transactions_.activeTransactionCount(documentId);
            },
            [this](const DocumentId& documentId) {
                return tasks_.activeExecutionCount(documentId);
            },
            [this](const DocumentId& documentId) {
                return workflows_.activeInstanceCount(documentId);
            },
            [this](const DocumentId& documentId) {
                return scripts_.activeInstanceCount(documentId);
            }});
    static_cast<void>(diagnostics_.addExporter(
        std::make_shared<PersistenceDiagnosticExporter>(persistence_)));
}

AppKernel::~AppKernel()
{
    if(state_ == AppKernelState::Ready || state_ == AppKernelState::Stopping) {
        static_cast<void>(shutdown());
    }
}

foundation::Result<void> AppKernel::addModule(std::unique_ptr<IModule> module)
{
    if(state_ != AppKernelState::Configuring) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.AppKernelNotConfiguring",
            foundation::ErrorCategory::Conflict,
            "Modules can only be added while the application kernel is configuring"));
    }
    return modules_.addModule(std::move(module));
}

foundation::Result<void> AppKernel::configureTaskExecutor(
    std::unique_ptr<platform::ITaskExecutor> executor)
{
    if(state_ != AppKernelState::Configuring) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.TaskExecutorNotConfiguring",
            foundation::ErrorCategory::Conflict,
            "The task executor can only be configured while the application kernel is configuring"));
    }
    if(executor == nullptr) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.InvalidTaskExecutor",
            foundation::ErrorCategory::Validation,
            "A task executor is required"));
    }
    auto configured = scheduler_.configureExecutor(*executor);
    if(!configured) {
        return configured;
    }
    taskExecutor_ = std::move(executor);
    return foundation::Result<void>::success();
}

foundation::Result<void> AppKernel::bootstrap()
{
    if(state_ != AppKernelState::Configuring) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.AppKernelAlreadyBootstrapped",
            foundation::ErrorCategory::Conflict,
            "The application kernel can only be bootstrapped once"));
    }

    state_ = AppKernelState::Starting;
    if(persistence_.configured()) {
        auto initialized = persistence_.initialize();
        if(!initialized) {
            state_ = AppKernelState::Failed;
            return foundation::Result<void>::failure(foundation::makeError(
                "Persistence.KernelInitializationFailed",
                foundation::ErrorCategory::Infrastructure,
                "The application kernel could not initialize persistence",
                foundation::Value {},
                foundation::Severity::Error,
                std::make_shared<const foundation::Error>(
                    std::move(initialized).error())));
        }
        auto recovered = persistence_.recover();
        if(!recovered) {
            state_ = AppKernelState::Failed;
            return foundation::Result<void>::failure(foundation::makeError(
                "Persistence.KernelRecoveryFailed",
                foundation::ErrorCategory::Infrastructure,
                "The application kernel could not recover durable state",
                foundation::Value {},
                foundation::Severity::Error,
                std::make_shared<const foundation::Error>(
                    std::move(recovered).error())));
        }
        auto catalog = persistence_.documentCatalog();
        if(!catalog) {
            state_ = AppKernelState::Failed;
            return foundation::Result<void>::failure(foundation::makeError(
                "Persistence.KernelDocumentCatalogFailed",
                foundation::ErrorCategory::Infrastructure,
                "The application kernel could not recover the durable document catalog",
                foundation::Value {},
                foundation::Severity::Error,
                std::make_shared<const foundation::Error>(
                    std::move(catalog).error())));
        }

        auto restoredImages = recovered.value().documents;
        restoredImages.erase(
            std::remove_if(
                restoredImages.begin(),
                restoredImages.end(),
                [&](const state::DocumentImage& image) {
                    const auto durable = std::find_if(
                        catalog.value().begin(),
                        catalog.value().end(),
                        [&](const persistence::DocumentCatalogRecord& record) {
                            return record.documentId == image.documentId;
                        });
                    return durable != catalog.value().end()
                        && durable->state
                            != persistence::DocumentPersistenceState::Open;
                }),
            restoredImages.end());

        for(const auto& record : catalog.value()) {
            if(record.state != persistence::DocumentPersistenceState::Open
               || documents_.contains(record.documentId)
               || std::any_of(
                   restoredImages.begin(),
                   restoredImages.end(),
                   [&](const state::DocumentImage& image) {
                       return image.documentId == record.documentId;
                   })) {
                continue;
            }
            state::Revision projectRevision;
            for(const auto& image : recovered.value().documents) {
                if(image.projectId == record.projectId) {
                    projectRevision = image.revisions.at(
                        state::RevisionScope::Project);
                    break;
                }
            }
            restoredImages.push_back(state::DocumentImage {
                record.projectId,
                record.documentId,
                state::RevisionSet {
                    projectRevision,
                    state::Revision {},
                    state::Revision {},
                    state::Revision {},
                    state::Revision {},
                    state::Revision {}},
                {}});
        }

        auto restored = documents_.restoreDocuments(restoredImages);
        if(!restored) {
            state_ = AppKernelState::Failed;
            return foundation::Result<void>::failure(foundation::makeError(
                "Persistence.KernelRestoreFailed",
                foundation::ErrorCategory::Infrastructure,
                "The application kernel could not install recovered state",
                foundation::Value {},
                foundation::Severity::Error,
                std::make_shared<const foundation::Error>(
                    std::move(restored).error())));
        }
        auto adopted = documentRuntime_.adoptRecovered(restoredImages);
        if(!adopted) {
            state_ = AppKernelState::Failed;
            return foundation::Result<void>::failure(foundation::makeError(
                "Persistence.KernelDocumentLifecycleRestoreFailed",
                foundation::ErrorCategory::Infrastructure,
                "The application kernel could not install recovered document lifecycle state",
                foundation::Value {},
                foundation::Severity::Error,
                std::make_shared<const foundation::Error>(
                    std::move(adopted).error())));
        }
        auto catalogAdopted = documentRuntime_.adoptCatalog(catalog.value());
        if(!catalogAdopted) {
            state_ = AppKernelState::Failed;
            return foundation::Result<void>::failure(foundation::makeError(
                "Persistence.KernelDocumentCatalogRestoreFailed",
                foundation::ErrorCategory::Infrastructure,
                "The application kernel could not install durable document lifecycle state",
                foundation::Value {},
                foundation::Severity::Error,
                std::make_shared<const foundation::Error>(
                    std::move(catalogAdopted).error())));
        }
        for(const auto& lifecycle : documentRuntime_.list()) {
            if(lifecycle.state != runtime::DocumentLifecycleState::Open) {
                continue;
            }
            const auto durable = std::find_if(
                catalog.value().begin(),
                catalog.value().end(),
                [&](const persistence::DocumentCatalogRecord& record) {
                    return record.documentId == lifecycle.documentId;
                });
            if(durable != catalog.value().end()) {
                continue;
            }
            auto saved = persistence_.saveDocumentLifecycle(
                lifecycle.projectId,
                lifecycle.documentId,
                persistence::DocumentPersistenceState::Open);
            if(!saved) {
                state_ = AppKernelState::Failed;
                return foundation::Result<void>::failure(foundation::makeError(
                    "Persistence.KernelDocumentCatalogSyncFailed",
                    foundation::ErrorCategory::Infrastructure,
                    "The application kernel could not synchronize document lifecycle state",
                    foundation::Value {},
                    foundation::Severity::Error,
                    std::make_shared<const foundation::Error>(
                        std::move(saved).error())));
            }
        }
    }

    auto result = modules_.bootstrap(*this);
    if(!result) {
        state_ = AppKernelState::Failed;
        return result;
    }

    if((commandRegistry_.size() != 0U || queryRegistry_.size() != 0U
        || taskRegistry_.size() != 0U || workflowRegistry_.size() != 0U
        || scriptRegistry_.size() != 0U)
       && !executionServices_.configured()) {
        auto stopped = modules_.shutdown(*this);
        state_ = AppKernelState::Failed;
        return foundation::Result<void>::failure(foundation::makeError(
            "Runtime.ExecutionServicesNotConfigured",
            foundation::ErrorCategory::Conflict,
            "Registered commands and queries require schema validation and logging services",
            foundation::Value {},
            foundation::Severity::Error,
            stopped.hasValue()
                ? nullptr
                : std::make_shared<const foundation::Error>(std::move(stopped).error())));
    }
    if(taskRegistry_.size() != 0U && taskExecutor_ == nullptr) {
        auto stopped = modules_.shutdown(*this);
        state_ = AppKernelState::Failed;
        return foundation::Result<void>::failure(foundation::makeError(
            "Task.ExecutorNotConfigured",
            foundation::ErrorCategory::Conflict,
            "Registered tasks require a configured task executor",
            foundation::Value {},
            foundation::Severity::Error,
            stopped.hasValue()
                ? nullptr
                : std::make_shared<const foundation::Error>(std::move(stopped).error())));
    }

    auto effectsValidated = effects_.validate(commandRegistry_.descriptors());
    if(!effectsValidated) {
        auto stopped = modules_.shutdown(*this);
        state_ = AppKernelState::Failed;
        return foundation::Result<void>::failure(foundation::makeError(
            "Effect.RegistryValidationFailed",
            foundation::ErrorCategory::Validation,
            "The external-effect execution contract could not be frozen",
            foundation::Value {},
            foundation::Severity::Error,
            std::make_shared<const foundation::Error>(
                stopped.hasValue() ? std::move(effectsValidated).error()
                                   : std::move(stopped).error())));
    }

    auto workflowsValidated = workflowRegistry_.validateAndFreeze();
    if(!workflowsValidated) {
        auto stopped = modules_.shutdown(*this);
        state_ = AppKernelState::Failed;
        return foundation::Result<void>::failure(foundation::makeError(
            "Workflow.RegistryValidationFailed",
            foundation::ErrorCategory::Validation,
            "The workflow registry could not be frozen",
            foundation::Value {},
            foundation::Severity::Error,
            std::make_shared<const foundation::Error>(
                stopped.hasValue() ? std::move(workflowsValidated).error()
                                   : std::move(stopped).error())));
    }

    auto scriptsValidated = scriptRegistry_.validateAndFreeze();
    if(!scriptsValidated) {
        auto stopped = modules_.shutdown(*this);
        state_ = AppKernelState::Failed;
        return foundation::Result<void>::failure(foundation::makeError(
            "Script.RegistryValidationFailed",
            foundation::ErrorCategory::Validation,
            "The script registry could not be frozen",
            foundation::Value {},
            foundation::Severity::Error,
            std::make_shared<const foundation::Error>(
                stopped.hasValue() ? std::move(scriptsValidated).error()
                                   : std::move(stopped).error())));
    }

    auto workflowsRestored = workflows_.restore();
    if(!workflowsRestored) {
        auto stopped = modules_.shutdown(*this);
        state_ = AppKernelState::Failed;
        return foundation::Result<void>::failure(foundation::makeError(
            "Workflow.KernelRecoveryFailed",
            foundation::ErrorCategory::Infrastructure,
            "The application kernel could not recover durable workflows",
            foundation::Value {},
            foundation::Severity::Error,
            std::make_shared<const foundation::Error>(
                stopped.hasValue() ? std::move(workflowsRestored).error()
                                   : std::move(stopped).error())));
    }

    if(taskExecutor_ != nullptr) {
        auto scheduled = scheduler_.start();
        if(!scheduled) {
            auto stopped = modules_.shutdown(*this);
            state_ = AppKernelState::Failed;
            return foundation::Result<void>::failure(foundation::makeError(
                "Task.SchedulerStartFailed",
                foundation::ErrorCategory::Infrastructure,
                "The task scheduler could not start",
                foundation::Value {},
                foundation::Severity::Error,
                std::make_shared<const foundation::Error>(std::move(scheduled).error())));
        }
    }

    services_.freeze();
    executionServices_.freeze();
    commandRegistry_.freeze();
    effectGuards_.freeze();
    queryRegistry_.freeze();
    taskRegistry_.freeze();
    resources_.freeze();
    persistence_.freeze();
    traces_.freeze();
    metrics_.freeze();
    diagnostics_.freeze();
    documentRuntime_.start();
    commands_.start();
    queries_.start();
    if(taskExecutor_ != nullptr) {
        tasks_.start();
    }
    workflows_.start();
    scripts_.start();
    state_ = AppKernelState::Ready;
    return foundation::Result<void>::success();
}

foundation::Result<void> AppKernel::shutdown(std::chrono::milliseconds taskTimeout)
{
    if(state_ == AppKernelState::Stopped) {
        return foundation::Result<void>::success();
    }
    if(state_ != AppKernelState::Ready && state_ != AppKernelState::Configuring
       && state_ != AppKernelState::Stopping) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.AppKernelCannotStop",
            foundation::ErrorCategory::Conflict,
            "The application kernel cannot stop from its current state"));
    }
    const auto activeTransactionCount = transactions_.activeTransactionCount();
    if(activeTransactionCount != 0U) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.ActiveTransactions",
            foundation::ErrorCategory::Conflict,
            "The application kernel cannot stop while transactions are active",
            foundation::Value {foundation::Value::Object {
                {"activeTransactionCount",
                 foundation::Value {std::to_string(activeTransactionCount)}},
            }}));
    }
    const auto activeCommandCount = commands_.activeExecutionCount();
    const auto activeQueryCount = queries_.activeExecutionCount();
    const auto activeWorkflowCount = workflows_.activeExecutionCount();
    const auto activeScriptCount = scripts_.activeExecutionCount();
    if(activeCommandCount != 0U || activeQueryCount != 0U || activeWorkflowCount != 0U
       || activeScriptCount != 0U) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.ActiveExecutions",
            foundation::ErrorCategory::Conflict,
            "The application kernel cannot stop while runtime executions are active",
            foundation::Value {foundation::Value::Object {
                {"activeCommandCount", foundation::Value {std::to_string(activeCommandCount)}},
                {"activeQueryCount", foundation::Value {std::to_string(activeQueryCount)}},
                {"activeWorkflowCount", foundation::Value {std::to_string(activeWorkflowCount)}},
                {"activeScriptCount", foundation::Value {std::to_string(activeScriptCount)}},
            }}));
    }

    const bool wasConfiguring = state_ == AppKernelState::Configuring;
    state_ = AppKernelState::Stopping;
    documentRuntime_.stop();
    scripts_.stop();
    workflows_.stop();
    commands_.stop();
    queries_.stop();
    tasks_.stop();
    if(taskExecutor_ != nullptr) {
        auto tasksStopped = wasConfiguring ? taskExecutor_->shutdown()
                                           : scheduler_.shutdown(taskTimeout);
        if(!tasksStopped) {
            return tasksStopped;
        }
    }
    auto result = modules_.shutdown(*this);
    if(!result) {
        state_ = AppKernelState::Failed;
        return result;
    }

    state_ = AppKernelState::Stopped;
    return foundation::Result<void>::success();
}

ServiceRegistry& AppKernel::services() noexcept
{
    return services_;
}

const ServiceRegistry& AppKernel::services() const noexcept
{
    return services_;
}

ModuleRuntime& AppKernel::modules() noexcept
{
    return modules_;
}

const ModuleRuntime& AppKernel::modules() const noexcept
{
    return modules_;
}

foundation::Result<void> AppKernel::addDocument(
    ProjectId projectId,
    DocumentId documentId)
{
    if(state_ != AppKernelState::Configuring) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.DocumentLoadNotConfiguring",
            foundation::ErrorCategory::Conflict,
            "Documents can only be attached while the application kernel is configuring"));
    }
    return documentRuntime_.configureDocument(
        std::move(projectId), std::move(documentId));
}

const state::DocumentStore& AppKernel::documents() const noexcept
{
    return documents_;
}

runtime::DocumentRuntime& AppKernel::documentRuntime() noexcept
{
    return documentRuntime_;
}

const runtime::DocumentRuntime& AppKernel::documentRuntime() const noexcept
{
    return documentRuntime_;
}

runtime::ExecutionServices& AppKernel::executionServices() noexcept
{
    return executionServices_;
}

const runtime::ExecutionServices& AppKernel::executionServices() const noexcept
{
    return executionServices_;
}

runtime::CapabilityService& AppKernel::capabilities() noexcept
{
    return capabilities_;
}

const runtime::CapabilityService& AppKernel::capabilities() const noexcept
{
    return capabilities_;
}

messaging::EventBus& AppKernel::events() noexcept
{
    return events_;
}

const messaging::EventBus& AppKernel::events() const noexcept
{
    return events_;
}

runtime::CommandRegistry& AppKernel::commandRegistry() noexcept
{
    return commandRegistry_;
}

const runtime::CommandRegistry& AppKernel::commandRegistry() const noexcept
{
    return commandRegistry_;
}

runtime::EffectGuardRegistry& AppKernel::effectGuards() noexcept
{
    return effectGuards_;
}

const runtime::EffectGuardRegistry& AppKernel::effectGuards() const noexcept
{
    return effectGuards_;
}

runtime::QueryRegistry& AppKernel::queryRegistry() noexcept
{
    return queryRegistry_;
}

const runtime::QueryRegistry& AppKernel::queryRegistry() const noexcept
{
    return queryRegistry_;
}

runtime::WorkflowRegistry& AppKernel::workflowRegistry() noexcept
{
    return workflowRegistry_;
}

const runtime::WorkflowRegistry& AppKernel::workflowRegistry() const noexcept
{
    return workflowRegistry_;
}

runtime::ScriptRegistry& AppKernel::scriptRegistry() noexcept
{
    return scriptRegistry_;
}

const runtime::ScriptRegistry& AppKernel::scriptRegistry() const noexcept
{
    return scriptRegistry_;
}

runtime::CommandRuntime& AppKernel::commands() noexcept
{
    return commands_;
}

const runtime::CommandRuntime& AppKernel::commands() const noexcept
{
    return commands_;
}

runtime::QueryRuntime& AppKernel::queries() noexcept
{
    return queries_;
}

const runtime::QueryRuntime& AppKernel::queries() const noexcept
{
    return queries_;
}

runtime::TaskRegistry& AppKernel::taskRegistry() noexcept
{
    return taskRegistry_;
}

const runtime::TaskRegistry& AppKernel::taskRegistry() const noexcept
{
    return taskRegistry_;
}

runtime::ResourceManager& AppKernel::resources() noexcept
{
    return resources_;
}

const runtime::ResourceManager& AppKernel::resources() const noexcept
{
    return resources_;
}

runtime::Scheduler& AppKernel::scheduler() noexcept
{
    return scheduler_;
}

const runtime::Scheduler& AppKernel::scheduler() const noexcept
{
    return scheduler_;
}

runtime::TaskRuntime& AppKernel::tasks() noexcept
{
    return tasks_;
}

const runtime::TaskRuntime& AppKernel::tasks() const noexcept
{
    return tasks_;
}

runtime::WorkflowRuntime& AppKernel::workflows() noexcept
{
    return workflows_;
}

const runtime::WorkflowRuntime& AppKernel::workflows() const noexcept
{
    return workflows_;
}

runtime::ScriptRuntime& AppKernel::scripts() noexcept
{
    return scripts_;
}

const runtime::ScriptRuntime& AppKernel::scripts() const noexcept
{
    return scripts_;
}

observability::LocalTraceService& AppKernel::traces() noexcept
{
    return traces_;
}

const observability::LocalTraceService& AppKernel::traces() const noexcept
{
    return traces_;
}

observability::LocalMetricsService& AppKernel::metrics() noexcept
{
    return metrics_;
}

const observability::LocalMetricsService& AppKernel::metrics() const noexcept
{
    return metrics_;
}

observability::DiagnosticsService& AppKernel::diagnostics() noexcept
{
    return diagnostics_;
}

const observability::DiagnosticsService& AppKernel::diagnostics() const noexcept
{
    return diagnostics_;
}

persistence::PersistenceService& AppKernel::persistence() noexcept
{
    return persistence_;
}

const persistence::PersistenceService& AppKernel::persistence() const noexcept
{
    return persistence_;
}

AppKernelState AppKernel::state() const noexcept
{
    return state_;
}

} // namespace lasercnc::kernel
