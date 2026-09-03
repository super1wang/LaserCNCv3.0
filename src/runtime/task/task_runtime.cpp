#include <lasercnc/runtime/task_runtime.hpp>

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/persistence/persistence_service.hpp>
#include <lasercnc/runtime/document_runtime.hpp>

#include <string>
#include <utility>

namespace lasercnc::runtime {
namespace {

class ValidatingTaskHandler final : public ITaskHandler {
public:
    ValidatingTaskHandler(
        std::shared_ptr<ITaskHandler> handler,
        foundation::Schema resultSchema,
        std::shared_ptr<foundation::ISchemaValidator> validator)
        : handler_(std::move(handler)),
          resultSchema_(std::move(resultSchema)),
          validator_(std::move(validator))
    {
    }

    foundation::Result<foundation::Value> execute(
        const TaskRequest& request,
        const TaskContext& context) override
    {
        auto result = handler_->execute(request, context);
        if(!result) {
            return result;
        }
        auto validated = validator_->validate(resultSchema_, result.value());
        if(!validated) {
            return foundation::Result<foundation::Value>::failure(std::move(validated).error());
        }
        return result;
    }

private:
    std::shared_ptr<ITaskHandler> handler_;
    foundation::Schema resultSchema_;
    std::shared_ptr<foundation::ISchemaValidator> validator_;
};

} // namespace

TaskRuntime::TaskRuntime(
    TaskRegistry& registry,
    Scheduler& scheduler,
    ExecutionServices& executionServices,
    const state::DocumentStore& documents,
    DocumentRuntime* documentRuntime)
    : registry_(registry),
      scheduler_(scheduler),
      executionServices_(executionServices),
      documents_(documents),
      documentRuntime_(documentRuntime)
{
}

TaskRuntime::TaskRuntime(
    TaskRegistry& registry,
    Scheduler& scheduler,
    ExecutionServices& executionServices,
    const state::DocumentStore& documents,
    persistence::PersistenceService& persistence,
    DocumentRuntime* documentRuntime)
    : registry_(registry),
      scheduler_(scheduler),
      executionServices_(executionServices),
      documents_(documents),
      persistence_(&persistence),
      documentRuntime_(documentRuntime)
{
}

void TaskRuntime::start() noexcept
{
    accepting_.store(true, std::memory_order_release);
}

void TaskRuntime::stop() noexcept
{
    accepting_.store(false, std::memory_order_release);
}

foundation::Result<void> TaskRuntime::submit(TaskRequest request)
{
    return submit(std::move(request), std::nullopt);
}

foundation::Result<void> TaskRuntime::submit(
    TaskRequest request,
    std::optional<TransactionIdempotency> commandIdempotency)
{
    if(!accepting_.load(std::memory_order_acquire)) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Task.RuntimeNotAccepting",
            foundation::ErrorCategory::Conflict,
            "The task runtime is not accepting new work"));
    }
    std::optional<DocumentActivityLease> documentActivity;
    if(documentRuntime_ != nullptr && request.documentId.has_value()) {
        auto admitted = documentRuntime_->acquireActivity(
            *request.documentId, DocumentActivityKind::TaskAdmission);
        if(!admitted) {
            return foundation::Result<void>::failure(std::move(admitted).error());
        }
        documentActivity.emplace(std::move(admitted).value());
    }
    auto entry = registry_.resolve(request.task);
    if(!entry) {
        return foundation::Result<void>::failure(std::move(entry).error());
    }
    auto resolved = std::move(entry).value();
    auto services = executionServices_.snapshot();
    if(!services) {
        return foundation::Result<void>::failure(std::move(services).error());
    }
    auto inputValidated = services.value().schemaValidator->validate(
        resolved.descriptor.input, request.input);
    if(!inputValidated) {
        return inputValidated;
    }
    auto validatingHandler = std::make_shared<ValidatingTaskHandler>(
        std::move(resolved.handler),
        resolved.descriptor.result,
        services.value().schemaValidator);

    std::optional<state::Document> document;
    std::optional<state::RevisionSet> sourceRevisions;
    std::function<bool()> sourceIsStale;
    if(request.documentId.has_value()) {
        if(!request.projectId.has_value()) {
            return foundation::Result<void>::failure(foundation::makeError(
                "Task.ProjectRequired",
                foundation::ErrorCategory::Validation,
                "A document task must declare its owning project"));
        }
        auto captured = documents_.snapshot(*request.documentId);
        if(!captured) {
            return foundation::Result<void>::failure(std::move(captured).error());
        }
        if(captured.value().projectId() != *request.projectId) {
            return foundation::Result<void>::failure(foundation::makeError(
                "Task.ProjectMismatch",
                foundation::ErrorCategory::Conflict,
                "The task project does not own the requested document"));
        }
        if(request.expectedRevisions.has_value()
           && *request.expectedRevisions != captured.value().revisions()) {
            return foundation::Result<void>::failure(foundation::makeError(
                "Task.RevisionConflict",
                foundation::ErrorCategory::Conflict,
                "The task source revisions do not match the caller precondition"));
        }
        if(request.expectedProjectRevision.has_value()
           && *request.expectedProjectRevision
               != captured.value().revisions().at(state::RevisionScope::Project)) {
            return foundation::Result<void>::failure(foundation::makeError(
                "Task.RevisionConflict",
                foundation::ErrorCategory::Conflict,
                "The task project revision does not match the caller precondition"));
        }
        const auto documentId = *request.documentId;
        const auto revisions = captured.value().revisions();
        sourceRevisions = revisions;
        const auto* documents = &documents_;
        sourceIsStale = [documents, documentId, revisions]() {
            auto current = documents->snapshot(documentId);
            return !current || current.value().revisions() != revisions;
        };
        document = std::move(captured).value();
    } else if(request.expectedRevisions.has_value()
              || request.expectedProjectRevision.has_value()) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Task.DocumentRequiredForRevision",
            foundation::ErrorCategory::Validation,
            "Revision preconditions require a document task"));
    }
    const bool durable = persistence_ != nullptr && persistence_->configured();
    std::optional<TaskRequest> durableRequest;
    if(durable) {
        durableRequest = request;
    }
    const auto taskId = request.taskId;
    auto scheduled = scheduler_.schedule(
        std::move(resolved.descriptor),
        std::move(validatingHandler),
        std::move(request),
        std::move(document),
        std::move(sourceIsStale),
        !durable);
    if(!scheduled) {
        return scheduled;
    }
    if(!durable) {
        return foundation::Result<void>::success();
    }
    auto persisted = persistence_->acceptTask(
        *durableRequest, sourceRevisions, commandIdempotency);
    if(!persisted) {
        scheduler_.discardPrepared(taskId);
        return persisted;
    }
    return scheduler_.activate(taskId);
}

foundation::Result<void> TaskRuntime::cancel(const kernel::TaskId& taskId)
{
    return scheduler_.requestCancel(taskId);
}

foundation::Result<TaskSnapshot> TaskRuntime::snapshot(const kernel::TaskId& taskId) const
{
    auto current = scheduler_.snapshot(taskId);
    if(current || persistence_ == nullptr || !persistence_->configured()
       || std::string(current.error().code.value()) != "Task.IdNotFound") {
        return current;
    }
    auto durable = persistence_->taskHistory(taskId);
    if(!durable) {
        return foundation::Result<TaskSnapshot>::failure(std::move(durable).error());
    }
    if(!durable.value().has_value()) {
        return current;
    }
    return foundation::Result<TaskSnapshot>::success(
        std::move(*durable.value()));
}

foundation::Result<TaskSnapshot> TaskRuntime::wait(
    const kernel::TaskId& taskId,
    std::chrono::milliseconds timeout) const
{
    auto current = scheduler_.wait(taskId, timeout);
    if(current || persistence_ == nullptr || !persistence_->configured()
       || std::string(current.error().code.value()) != "Task.IdNotFound") {
        return current;
    }
    auto durable = persistence_->taskHistory(taskId);
    if(!durable) {
        return foundation::Result<TaskSnapshot>::failure(std::move(durable).error());
    }
    if(!durable.value().has_value()) {
        return current;
    }
    return foundation::Result<TaskSnapshot>::success(
        std::move(*durable.value()));
}

std::size_t TaskRuntime::activeExecutionCount() const
{
    return scheduler_.activeTaskCount();
}

std::size_t TaskRuntime::activeExecutionCount(
    const kernel::DocumentId& documentId) const
{
    return scheduler_.activeTaskCount(documentId);
}

} // namespace lasercnc::runtime
