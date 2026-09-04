#include <lasercnc/kernel/app_kernel.hpp>

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/runtime/asset_validation.hpp>

#include <algorithm>
#include <set>
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
    : projectRuntime_(persistence_),
      documentRuntime_(documents_, persistence_, &objectTypes_),
      history_(documents_),
      transactions_(documents_, &persistence_, &documentRuntime_, &history_, &objectTypes_),
      workflowRegistry_(commandRegistry_, queryRegistry_),
      scriptRegistry_(commandRegistry_, queryRegistry_, workflowRegistry_),
      modules_(
          services_,
          commandRegistry_,
          queryRegistry_,
          taskRegistry_,
          workflowRegistry_,
          scriptRegistry_,
          objectTypes_),
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
          &documentRuntime_),
      executionGateway_(
          modules_,
          commandRegistry_,
          queryRegistry_,
          taskRegistry_,
          workflowRegistry_,
          scriptRegistry_,
          objectTypes_,
          commands_,
          queries_,
          tasks_,
          workflows_,
          scripts_)
{
    projectRuntime_.documents_ = &documentRuntime_;
    documentRuntime_.projects_ = &projectRuntime_;
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

ExecutionGateway& AppKernel::execution() noexcept
{
    return executionGateway_;
}

const ExecutionGateway& AppKernel::execution() const noexcept
{
    return executionGateway_;
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

foundation::Result<void> AppKernel::configureAssetStore(std::shared_ptr<platform::IAssetStore> store)
{
    if(state_ != AppKernelState::Configuring || assetStore_ != nullptr || store == nullptr) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Asset.InvalidKernelConfiguration", foundation::ErrorCategory::Conflict,
            "A non-null asset store may be configured once before kernel startup"));
    }
    assetStore_ = std::move(store);
    transactions_.assetStore_ = assetStore_.get();
    documentRuntime_.assetStore_ = assetStore_.get();
    return foundation::Result<void>::success();
}

foundation::Result<void> AppKernel::restoreState()
{
    objectTypes_.freeze();
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

        for(const auto& image : recovered.value().documents) {
            auto admitted = objectTypes_.validateObjects(image.objects, true);
            if(admitted) {
                admitted = runtime::validateObjectAssets(image.objects, assetStore_.get());
            }
            if(!admitted) {
                return foundation::Result<void>::failure(foundation::makeError(
                    "ObjectType.RecoveryAdmissionFailed", foundation::ErrorCategory::Validation,
                    "Recovered document state failed exact type and reference admission",
                    foundation::Value {}, foundation::Severity::Error,
                    std::make_shared<const foundation::Error>(std::move(admitted).error())));
            }
        }
        for(const auto& commit : recovered.value().historyCommits) {
            for(const auto& change : commit.changes) {
                for(const auto* record : {&change.before, &change.after}) {
                    if(!record->has_value()) {
                        continue;
                    }
                    auto admitted = objectTypes_.validateRecord(**record, true);
                    if(admitted) {
                        admitted = runtime::validateObjectAssets(std::span{&**record, 1U}, assetStore_.get());
                    }
                    if(!admitted) {
                        return foundation::Result<void>::failure(foundation::makeError(
                            "ObjectType.HistoryAdmissionFailed", foundation::ErrorCategory::Validation,
                            "Recovered history material failed exact type admission",
                            foundation::Value {}, foundation::Severity::Error,
                            std::make_shared<const foundation::Error>(std::move(admitted).error())));
                    }
                }
            }
        }
        // Only verified durable roots can seed the once-only legacy migration.
        // 中文翻译：只有已验证的持久文档根身份可以参与一次性旧目录迁移。
        std::set<ProjectId> durableRoots;
        for(const auto& image : recovered.value().documents) { durableRoots.insert(image.projectId); }
        for(const auto& record : catalog.value()) { durableRoots.insert(record.projectId); }
        const std::vector<ProjectId> migrationRoots(durableRoots.begin(), durableRoots.end());
        auto migrated = persistence_.completeProjectCatalogMigration(migrationRoots);
        if(!migrated) { return migrated; }
        auto projects = persistence_.projectCatalog();
        if(!projects) { return foundation::Result<void>::failure(std::move(projects).error()); }
        for(const auto& root : durableRoots) {
            if(std::none_of(projects.value().begin(), projects.value().end(),
                [&](const auto& record) { return record.projectId == root; })) {
                return foundation::Result<void>::failure(foundation::makeError(
                    "Project.RecoveryMissingRoot", foundation::ErrorCategory::Infrastructure,
                    "A durable document root has no project catalog entry"));
            }
        }
        auto projectsAdopted = projectRuntime_.adoptCatalog(projects.value());
        if(!projectsAdopted) { return projectsAdopted; }
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

        // An unavailable container must never expose recovered open children.
        // 中文翻译：不可用的项目容器不能暴露已恢复的 Open 子文档。
        for(const auto& image : restoredImages) {
            auto project = projectRuntime_.lifecycle(image.projectId);
            if(!project || project.value().state != runtime::ProjectLifecycleState::Open) {
                return foundation::Result<void>::failure(foundation::makeError(
                    "Project.RecoveryChildStateConflict", foundation::ErrorCategory::Infrastructure,
                    "An open durable document belongs to an unavailable project"));
            }
        }
        // Explicit startup composition may introduce new empty project identities.
        // 中文翻译：显式启动组合可声明新的空项目，但不能修复缺失的持久根身份。
        for(const auto& project : projectRuntime_.list()) {
            if(std::none_of(projects.value().begin(), projects.value().end(),
                [&](const auto& record) { return record.projectId == project.projectId; })) {
                auto saved = persistence_.saveProjectLifecycle(project.projectId, persistence::ProjectPersistenceState::Open);
                if(!saved) { return saved; }
            }
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
        auto historyRestored = history_.restore(recovered.value().historyCommits);
        if(!historyRestored) {
            state_ = AppKernelState::Failed;
            return foundation::Result<void>::failure(foundation::makeError(
                "History.KernelRecoveryFailed",
                foundation::ErrorCategory::Infrastructure,
                "The application kernel could not install recovered history state",
                foundation::Value {},
                foundation::Severity::Error,
                std::make_shared<const foundation::Error>(
                    std::move(historyRestored).error())));
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
    auto result = modules_.bootstrap(*this, [this] { return restoreState(); });
    if(!result) {
        state_ = AppKernelState::Failed;
        return result;
    }

    if(executionServices_.configured()) {
        auto historyCommands = history_.registerCommands(commandRegistry_);
        if(!historyCommands) {
            auto stopped = modules_.shutdown(*this);
            state_ = AppKernelState::Failed;
            return foundation::Result<void>::failure(foundation::makeError(
                "History.CommandRegistrationFailed",
                foundation::ErrorCategory::Conflict,
                "The built-in history commands could not be registered",
                foundation::Value {},
                foundation::Severity::Error,
                std::make_shared<const foundation::Error>(
                    stopped.hasValue() ? std::move(historyCommands).error()
                                       : std::move(stopped).error())));
        }
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
    objectTypes_.freeze();
    resources_.freeze();
    persistence_.freeze();
    traces_.freeze();
    metrics_.freeze();
    diagnostics_.freeze();
    projectRuntime_.start();
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
    projectRuntime_.stop();
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

const ServiceRegistry& AppKernel::services() const noexcept
{
    return services_;
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
    auto configured = documentRuntime_.configureDocument(projectId, std::move(documentId));
    if(!configured) { return configured; }
    return projectRuntime_.configureProject(projectId);
}

foundation::Result<void> AppKernel::addProject(ProjectId projectId)
{
    if(state_ != AppKernelState::Configuring) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.ProjectLoadNotConfiguring", foundation::ErrorCategory::Conflict,
            "Projects can only be configured before kernel bootstrap"));
    }
    return projectRuntime_.configureProject(projectId);
}

runtime::ProjectRuntime& AppKernel::projectRuntime() noexcept { return projectRuntime_; }
const runtime::ProjectRuntime& AppKernel::projectRuntime() const noexcept { return projectRuntime_; }

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

const runtime::HistoryRuntime& AppKernel::history() const noexcept
{
    return history_;
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

const runtime::QueryRegistry& AppKernel::queryRegistry() const noexcept
{
    return queryRegistry_;
}

const runtime::WorkflowRegistry& AppKernel::workflowRegistry() const noexcept
{
    return workflowRegistry_;
}

const runtime::ScriptRegistry& AppKernel::scriptRegistry() const noexcept
{
    return scriptRegistry_;
}

const runtime::TaskRegistry& AppKernel::taskRegistry() const noexcept
{
    return taskRegistry_;
}

const state::ObjectTypeRegistry& AppKernel::objectTypes() const noexcept
{
    return objectTypes_;
}

runtime::ResourceManager& AppKernel::resources() noexcept
{
    return resources_;
}

const runtime::ResourceManager& AppKernel::resources() const noexcept
{
    return resources_;
}

const runtime::Scheduler& AppKernel::scheduler() const noexcept
{
    return scheduler_;
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

foundation::Result<void> AppKernel::configurePersistence(
    std::unique_ptr<platform::IPersistenceBackend> backend,
    std::shared_ptr<foundation::IValueSerializer> serializer,
    std::shared_ptr<platform::IHashService> hashes,
    std::unique_ptr<platform::ISnapshotStore> snapshotStore)
{
    if(state_ != AppKernelState::Configuring) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.PersistenceConfigurationClosed", foundation::ErrorCategory::Conflict,
            "Persistence can only be configured before kernel bootstrap"));
    }
    return persistence_.configure(std::move(backend), std::move(serializer),
        std::move(hashes), std::move(snapshotStore));
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
