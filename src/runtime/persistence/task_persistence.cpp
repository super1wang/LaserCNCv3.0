#include <lasercnc/persistence/persistence_service.hpp>

#include <lasercnc/foundation/error.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace lasercnc::persistence {
namespace {

constexpr std::array revisionScopes {
    state::RevisionScope::Project,
    state::RevisionScope::Document,
    state::RevisionScope::Geometry,
    state::RevisionScope::Cam,
    state::RevisionScope::MachineContext,
    state::RevisionScope::Environment,
};

foundation::Error taskPersistenceError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    foundation::Value::Object details = {},
    std::shared_ptr<const foundation::Error> cause = nullptr)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {std::move(details)},
        foundation::Severity::Error,
        std::move(cause));
}

std::span<const std::byte> bytes(std::string_view value) noexcept
{
    return {
        reinterpret_cast<const std::byte*>(value.data()),
        value.size()};
}

foundation::Result<void> rollback(
    platform::IPersistenceBackend& backend,
    foundation::Error primary)
{
    auto rolledBack = backend.rollbackTransaction();
    if(rolledBack) {
        return foundation::Result<void>::failure(std::move(primary));
    }
    auto rollbackError = std::move(rolledBack).error();
    return foundation::Result<void>::failure(taskPersistenceError(
        "Persistence.RollbackFailed",
        foundation::ErrorCategory::Infrastructure,
        "Task persistence failed and could not roll back",
        {{"rollbackCode", foundation::Value {std::string(rollbackError.code.value())}},
         {"rollbackMessage", foundation::Value {rollbackError.message}}},
        std::make_shared<const foundation::Error>(std::move(primary))));
}

const char* stateName(runtime::TaskState state) noexcept
{
    switch(state) {
    case runtime::TaskState::Pending: return "pending";
    case runtime::TaskState::Ready: return "ready";
    case runtime::TaskState::Running: return "running";
    case runtime::TaskState::Succeeded: return "succeeded";
    case runtime::TaskState::Failed: return "failed";
    case runtime::TaskState::CancelRequested: return "cancel_requested";
    case runtime::TaskState::Cancelled: return "cancelled";
    case runtime::TaskState::Stale: return "stale";
    }
    return "unknown";
}

foundation::Result<runtime::TaskState> parseTerminalState(std::string_view state)
{
    if(state == "succeeded") {
        return foundation::Result<runtime::TaskState>::success(
            runtime::TaskState::Succeeded);
    }
    if(state == "failed") {
        return foundation::Result<runtime::TaskState>::success(runtime::TaskState::Failed);
    }
    if(state == "cancelled") {
        return foundation::Result<runtime::TaskState>::success(
            runtime::TaskState::Cancelled);
    }
    if(state == "stale") {
        return foundation::Result<runtime::TaskState>::success(runtime::TaskState::Stale);
    }
    return foundation::Result<runtime::TaskState>::failure(taskPersistenceError(
        "Persistence.InvalidTaskState",
        foundation::ErrorCategory::Infrastructure,
        "A persisted task has an invalid terminal state"));
}

foundation::Value revisionsValue(const state::RevisionSet& revisions)
{
    foundation::Value::Object object;
    for(const auto scope : revisionScopes) {
        object.emplace(
            state::revisionScopeName(scope),
            foundation::Value {std::to_string(revisions.at(scope).value())});
    }
    return foundation::Value {std::move(object)};
}

foundation::Value optionalRevisions(
    const std::optional<state::RevisionSet>& revisions)
{
    return revisions.has_value() ? revisionsValue(*revisions) : foundation::Value {};
}

foundation::Value requestValue(
    const runtime::TaskRequest& request,
    const std::optional<state::RevisionSet>& sourceRevisions)
{
    foundation::Value::Array dependencies;
    dependencies.reserve(request.dependencies.size());
    for(const auto& dependency : request.dependencies) {
        dependencies.emplace_back(std::string(dependency.value()));
    }
    foundation::Value::Array resources;
    resources.reserve(request.resources.size());
    for(const auto& resource : request.resources) {
        resources.emplace_back(foundation::Value::Object {
            {"access", foundation::Value {
                resource.access == runtime::ResourceAccess::Exclusive
                    ? "exclusive"
                    : "shared"}},
            {"kind", foundation::Value {static_cast<std::int64_t>(resource.kind)}},
            {"resourceId", foundation::Value {std::string(resource.resource.value())}},
            {"units", foundation::Value {std::to_string(resource.units)}},
        });
    }
    return foundation::Value {foundation::Value::Object {
        {"correlationId", request.correlationId.has_value()
            ? foundation::Value {std::string(request.correlationId->value())}
            : foundation::Value {}},
        {"dependencies", foundation::Value {std::move(dependencies)}},
        {"documentId", request.documentId.has_value()
            ? foundation::Value {std::string(request.documentId->value())}
            : foundation::Value {}},
        {"deadlineSteadyNanoseconds", request.deadline.has_value()
            ? foundation::Value {std::to_string(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      request.deadline->time_since_epoch()).count())}
            : foundation::Value {}},
        {"expectedProjectRevision", request.expectedProjectRevision.has_value()
            ? foundation::Value {std::to_string(request.expectedProjectRevision->value())}
            : foundation::Value {}},
        {"expectedRevisions", optionalRevisions(request.expectedRevisions)},
        {"format", foundation::Value {"lasercnc.task-acceptance"}},
        {"hasDeadline", foundation::Value {request.deadline.has_value()}},
        {"input", request.input},
        {"priority", foundation::Value {static_cast<std::int64_t>(request.priority)}},
        {"projectId", request.projectId.has_value()
            ? foundation::Value {std::string(request.projectId->value())}
            : foundation::Value {}},
        {"resources", foundation::Value {std::move(resources)}},
        {"sourceRevisions", optionalRevisions(sourceRevisions)},
        {"task", foundation::Value {std::string(request.task.value())}},
        {"taskId", foundation::Value {std::string(request.taskId.value())}},
        {"traceId", foundation::Value {std::string(request.traceId.value())}},
        {"version", foundation::Value {std::int64_t {1}}},
    }};
}

foundation::Value errorValue(const std::optional<foundation::Error>& error)
{
    if(!error.has_value()) {
        return foundation::Value {};
    }
    return foundation::Value {foundation::Value::Object {
        {"category", foundation::Value {static_cast<std::int64_t>(error->category)}},
        {"code", foundation::Value {std::string(error->code.value())}},
        {"details", error->details},
        {"message", foundation::Value {error->message}},
        {"severity", foundation::Value {static_cast<std::int64_t>(error->severity)}},
    }};
}

foundation::Value terminalValue(const runtime::TaskSnapshot& snapshot)
{
    return foundation::Value {foundation::Value::Object {
        {"error", errorValue(snapshot.error)},
        {"format", foundation::Value {"lasercnc.task-terminal"}},
        {"progress", foundation::Value {snapshot.progress}},
        {"progressMessage", foundation::Value {snapshot.progressMessage}},
        {"result", snapshot.result.has_value() ? *snapshot.result : foundation::Value {}},
        {"sourceRevisions", optionalRevisions(snapshot.sourceRevisions)},
        {"state", foundation::Value {stateName(snapshot.state)}},
        {"task", foundation::Value {std::string(snapshot.task.value())}},
        {"taskId", foundation::Value {std::string(snapshot.taskId.value())}},
        {"traceId", foundation::Value {std::string(snapshot.traceId.value())}},
        {"version", foundation::Value {std::int64_t {1}}},
    }};
}

const foundation::Value* field(
    const foundation::Value::Object& object,
    std::string_view name) noexcept
{
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &found->second;
}

foundation::Result<std::string> textColumn(
    const platform::PersistenceRow& row,
    const char* name)
{
    const auto found = row.find(name);
    const auto* value = found == row.end() ? nullptr : found->second.getIf<std::string>();
    if(value == nullptr) {
        return foundation::Result<std::string>::failure(taskPersistenceError(
            "Persistence.InvalidTaskRow",
            foundation::ErrorCategory::Infrastructure,
            "A task history row contains a missing or invalid text column",
            {{"column", foundation::Value {name}}}));
    }
    return foundation::Result<std::string>::success(*value);
}

template <typename Id>
foundation::Result<Id> idText(std::string text, const char* fieldName)
{
    auto id = Id::create(std::move(text));
    if(!id) {
        return foundation::Result<Id>::failure(taskPersistenceError(
            "Persistence.InvalidTaskIdentity",
            foundation::ErrorCategory::Infrastructure,
            "Task history contains an invalid stable identity",
            {{"field", foundation::Value {fieldName}}},
            std::make_shared<const foundation::Error>(std::move(id).error())));
    }
    return id;
}

template <typename Id>
foundation::Result<Id> idColumn(const platform::PersistenceRow& row, const char* name)
{
    auto value = textColumn(row, name);
    if(!value) {
        return foundation::Result<Id>::failure(std::move(value).error());
    }
    return idText<Id>(std::move(value).value(), name);
}

foundation::Result<state::Revision> parseRevision(std::string_view text)
{
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if(parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size()) {
        return foundation::Result<state::Revision>::failure(taskPersistenceError(
            "Persistence.InvalidTaskPayload",
            foundation::ErrorCategory::Infrastructure,
            "A task history revision is invalid"));
    }
    return foundation::Result<state::Revision>::success(state::Revision {value});
}

foundation::Result<std::optional<state::RevisionSet>> optionalRevisionValue(
    const foundation::Value& value)
{
    if(value.kind() == foundation::Value::Kind::Null) {
        return foundation::Result<std::optional<state::RevisionSet>>::success(
            std::nullopt);
    }
    const auto* object = value.getIf<foundation::Value::Object>();
    if(object == nullptr) {
        return foundation::Result<std::optional<state::RevisionSet>>::failure(
            taskPersistenceError(
                "Persistence.InvalidTaskPayload",
                foundation::ErrorCategory::Infrastructure,
                "A task history revision set must be an object"));
    }
    std::array<state::Revision, revisionScopes.size()> values;
    for(std::size_t index = 0U; index < revisionScopes.size(); ++index) {
        const auto found = object->find(state::revisionScopeName(revisionScopes[index]));
        const auto* text = found == object->end()
            ? nullptr
            : found->second.getIf<std::string>();
        if(text == nullptr) {
            return foundation::Result<std::optional<state::RevisionSet>>::failure(
                taskPersistenceError(
                    "Persistence.InvalidTaskPayload",
                    foundation::ErrorCategory::Infrastructure,
                    "A task history revision set is incomplete"));
        }
        auto revision = parseRevision(*text);
        if(!revision) {
            return foundation::Result<std::optional<state::RevisionSet>>::failure(
                std::move(revision).error());
        }
        values[index] = revision.value();
    }
    return foundation::Result<std::optional<state::RevisionSet>>::success(
        state::RevisionSet {
            values[0], values[1], values[2], values[3], values[4], values[5]});
}

foundation::Result<void> validateDigest(
    std::string_view payload,
    const kernel::ContentDigest& digest,
    const platform::IHashService& hashes,
    const char* code)
{
    auto actual = hashes.digest(bytes(payload));
    if(!actual) {
        return foundation::Result<void>::failure(std::move(actual).error());
    }
    if(actual.value() != digest) {
        return foundation::Result<void>::failure(taskPersistenceError(
            code,
            foundation::ErrorCategory::Infrastructure,
            "A task history payload failed its content digest check"));
    }
    return foundation::Result<void>::success();
}

foundation::Result<runtime::TaskSnapshot> decodeTerminal(
    std::string_view payload,
    const kernel::TaskId& indexedTaskId,
    const kernel::TaskName& indexedTaskName,
    const foundation::IValueSerializer& serializer)
{
    auto decoded = serializer.deserialize(payload);
    if(!decoded) {
        return foundation::Result<runtime::TaskSnapshot>::failure(
            std::move(decoded).error());
    }
    const auto* root = decoded.value().getIf<foundation::Value::Object>();
    const auto* formatValue = root == nullptr ? nullptr : field(*root, "format");
    const auto* format = formatValue == nullptr
        ? nullptr
        : formatValue->getIf<std::string>();
    const auto* versionValue = root == nullptr ? nullptr : field(*root, "version");
    const auto* version = versionValue == nullptr
        ? nullptr
        : versionValue->getIf<std::int64_t>();
    const auto* taskIdValue = root == nullptr ? nullptr : field(*root, "taskId");
    const auto* taskIdText = taskIdValue == nullptr
        ? nullptr
        : taskIdValue->getIf<std::string>();
    const auto* taskNameValue = root == nullptr ? nullptr : field(*root, "task");
    const auto* taskNameText = taskNameValue == nullptr
        ? nullptr
        : taskNameValue->getIf<std::string>();
    const auto* stateValue = root == nullptr ? nullptr : field(*root, "state");
    const auto* stateText = stateValue == nullptr
        ? nullptr
        : stateValue->getIf<std::string>();
    const auto* progressValue = root == nullptr ? nullptr : field(*root, "progress");
    const auto* progress = progressValue == nullptr
        ? nullptr
        : progressValue->getIf<double>();
    const auto* messageValue = root == nullptr ? nullptr : field(*root, "progressMessage");
    const auto* message = messageValue == nullptr
        ? nullptr
        : messageValue->getIf<std::string>();
    const auto* traceValue = root == nullptr ? nullptr : field(*root, "traceId");
    const auto* traceText = traceValue == nullptr
        ? nullptr
        : traceValue->getIf<std::string>();
    const auto* revisionsValueField = root == nullptr
        ? nullptr
        : field(*root, "sourceRevisions");
    const auto* resultValue = root == nullptr ? nullptr : field(*root, "result");
    const auto* errorValueField = root == nullptr ? nullptr : field(*root, "error");
    if(format == nullptr || *format != "lasercnc.task-terminal" || version == nullptr
       || *version != 1 || taskIdText == nullptr
       || *taskIdText != indexedTaskId.value() || taskNameText == nullptr
       || *taskNameText != indexedTaskName.value() || stateText == nullptr
       || progress == nullptr || message == nullptr || traceText == nullptr
       || revisionsValueField == nullptr || resultValue == nullptr
       || errorValueField == nullptr) {
        return foundation::Result<runtime::TaskSnapshot>::failure(taskPersistenceError(
            "Persistence.InvalidTaskPayload",
            foundation::ErrorCategory::Infrastructure,
            "A terminal task history payload is incomplete or mismatched"));
    }
    auto state = parseTerminalState(*stateText);
    auto traceId = idText<kernel::TraceId>(*traceText, "traceId");
    auto revisions = optionalRevisionValue(*revisionsValueField);
    if(!state || !traceId || !revisions) {
        return foundation::Result<runtime::TaskSnapshot>::failure(taskPersistenceError(
            "Persistence.InvalidTaskPayload",
            foundation::ErrorCategory::Infrastructure,
            "A terminal task history payload contains invalid typed fields"));
    }
    std::optional<foundation::Value> result;
    if(resultValue->kind() != foundation::Value::Kind::Null) {
        result = *resultValue;
    }
    std::optional<foundation::Error> error;
    if(errorValueField->kind() != foundation::Value::Kind::Null) {
        const auto* persistedError = errorValueField->getIf<foundation::Value::Object>();
        const auto* codeValue = persistedError == nullptr
            ? nullptr
            : field(*persistedError, "code");
        const auto* code = codeValue == nullptr
            ? nullptr
            : codeValue->getIf<std::string>();
        const auto* categoryValue = persistedError == nullptr
            ? nullptr
            : field(*persistedError, "category");
        const auto* category = categoryValue == nullptr
            ? nullptr
            : categoryValue->getIf<std::int64_t>();
        const auto* severityValue = persistedError == nullptr
            ? nullptr
            : field(*persistedError, "severity");
        const auto* severity = severityValue == nullptr
            ? nullptr
            : severityValue->getIf<std::int64_t>();
        const auto* errorMessageValue = persistedError == nullptr
            ? nullptr
            : field(*persistedError, "message");
        const auto* errorMessage = errorMessageValue == nullptr
            ? nullptr
            : errorMessageValue->getIf<std::string>();
        const auto* details = persistedError == nullptr
            ? nullptr
            : field(*persistedError, "details");
        if(code == nullptr || category == nullptr || severity == nullptr
           || errorMessage == nullptr || details == nullptr || *category < 0
           || *category > static_cast<std::int64_t>(foundation::ErrorCategory::Internal)
           || *severity < 0
           || *severity > static_cast<std::int64_t>(foundation::Severity::Fatal)) {
            return foundation::Result<runtime::TaskSnapshot>::failure(
                taskPersistenceError(
                    "Persistence.InvalidTaskPayload",
                    foundation::ErrorCategory::Infrastructure,
                    "A terminal task error payload is invalid"));
        }
        error = foundation::makeError(
            *code,
            static_cast<foundation::ErrorCategory>(*category),
            *errorMessage,
            *details,
            static_cast<foundation::Severity>(*severity));
    }
    return foundation::Result<runtime::TaskSnapshot>::success(runtime::TaskSnapshot {
        indexedTaskId,
        indexedTaskName,
        state.value(),
        *progress,
        *message,
        std::move(traceId).value(),
        std::move(revisions).value(),
        std::move(result),
        std::move(error)});
}

} // namespace

foundation::Result<void> PersistenceService::acceptTask(
    const runtime::TaskRequest& request,
    const std::optional<state::RevisionSet>& sourceRevisions,
    const std::optional<runtime::TransactionIdempotency>& commandIdempotency)
{
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<void>::failure(taskPersistenceError(
            "Persistence.NotReady",
            foundation::ErrorCategory::Conflict,
            "Persistence must be initialized before accepting durable tasks"));
    }
    bool transactionOpen = false;
    try {
        auto payload = serializer_->serialize(requestValue(request, sourceRevisions));
        if(!payload) {
            return foundation::Result<void>::failure(std::move(payload).error());
        }
        auto digest = hashes_->digest(bytes(payload.value()));
        if(!digest) {
            return foundation::Result<void>::failure(std::move(digest).error());
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if(now < 0) {
            return foundation::Result<void>::failure(taskPersistenceError(
                "Persistence.InvalidTimestamp",
                foundation::ErrorCategory::Internal,
                "The system clock produced an unsupported task timestamp"));
        }
        auto begun = backend_->beginTransaction();
        if(!begun) {
            return foundation::Result<void>::failure(std::move(begun).error());
        }
        transactionOpen = true;
        const std::array queryParameters {
            foundation::Value {std::string(request.taskId.value())}};
        auto rows = backend_->query(
            "SELECT task_name,request_payload,request_digest FROM task_history "
            "WHERE task_id=?",
            queryParameters);
        if(!rows) {
            auto failure = rollback(*backend_, std::move(rows).error());
            return foundation::Result<void>::failure(std::move(failure).error());
        }
        if(rows.value().empty()) {
            const std::array insertParameters {
                foundation::Value {std::string(request.taskId.value())},
                foundation::Value {std::string(request.task.value())},
                foundation::Value {payload.value()},
                foundation::Value {std::string(digest.value().value())},
                foundation::Value {now},
                foundation::Value {now}};
            auto inserted = backend_->execute(
                "INSERT INTO task_history(task_id,task_name,status,request_payload,"
                "request_digest,created_at_ms,updated_at_ms) "
                "VALUES(?,?,'accepted',?,?,?,?)",
                insertParameters);
            if(!inserted) {
                auto failure = rollback(*backend_, std::move(inserted).error());
                return foundation::Result<void>::failure(std::move(failure).error());
            }
        } else if(rows.value().size() == 1U) {
            auto taskName = idColumn<kernel::TaskName>(rows.value().front(), "task_name");
            auto storedPayload = textColumn(rows.value().front(), "request_payload");
            auto storedDigest = idColumn<kernel::ContentDigest>(
                rows.value().front(), "request_digest");
            if(!taskName || !storedPayload || !storedDigest
               || taskName.value() != request.task
               || storedPayload.value() != payload.value()
               || storedDigest.value() != digest.value()) {
                auto failure = rollback(*backend_, taskPersistenceError(
                    "Persistence.TaskIdentityConflict",
                    foundation::ErrorCategory::Conflict,
                    "A task identity is already bound to different durable input"));
                return foundation::Result<void>::failure(std::move(failure).error());
            }
        } else {
            auto failure = rollback(*backend_, taskPersistenceError(
                "Persistence.InvalidTaskRow",
                foundation::ErrorCategory::Infrastructure,
                "A task identity resolved ambiguously"));
            return foundation::Result<void>::failure(std::move(failure).error());
        }
        if(commandIdempotency.has_value()) {
            auto completed = completeTaskCommandInOpenTransaction(
                request, *commandIdempotency);
            if(!completed) {
                auto failure = rollback(*backend_, std::move(completed).error());
                return foundation::Result<void>::failure(std::move(failure).error());
            }
        }
        auto committed = backend_->commitTransaction();
        if(!committed) {
            auto failure = rollback(*backend_, std::move(committed).error());
            return foundation::Result<void>::failure(std::move(failure).error());
        }
        transactionOpen = false;
        return foundation::Result<void>::success();
    } catch(const std::exception& exception) {
        auto error = taskPersistenceError(
            "Persistence.TaskAcceptFailed",
            foundation::ErrorCategory::Internal,
            "Durable task acceptance failed unexpectedly",
            {{"reason", foundation::Value {exception.what()}}});
        return transactionOpen ? rollback(*backend_, std::move(error))
                               : foundation::Result<void>::failure(std::move(error));
    } catch(...) {
        auto error = taskPersistenceError(
            "Persistence.TaskAcceptFailed",
            foundation::ErrorCategory::Internal,
            "Durable task acceptance failed unexpectedly");
        return transactionOpen ? rollback(*backend_, std::move(error))
                               : foundation::Result<void>::failure(std::move(error));
    }
}

foundation::Result<void> PersistenceService::recordTaskTerminal(
    const runtime::TaskSnapshot& snapshot)
{
    if(!runtime::isTerminal(snapshot.state)) {
        return foundation::Result<void>::failure(taskPersistenceError(
            "Persistence.TaskNotTerminal",
            foundation::ErrorCategory::Validation,
            "Only terminal task snapshots can be persisted as outcomes"));
    }
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<void>::failure(taskPersistenceError(
            "Persistence.NotReady",
            foundation::ErrorCategory::Conflict,
            "Persistence must be initialized before recording task outcomes"));
    }
    bool transactionOpen = false;
    try {
        auto payload = serializer_->serialize(terminalValue(snapshot));
        if(!payload) {
            return foundation::Result<void>::failure(std::move(payload).error());
        }
        auto digest = hashes_->digest(bytes(payload.value()));
        if(!digest) {
            return foundation::Result<void>::failure(std::move(digest).error());
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if(now < 0) {
            return foundation::Result<void>::failure(taskPersistenceError(
                "Persistence.InvalidTimestamp",
                foundation::ErrorCategory::Internal,
                "The system clock produced an unsupported task timestamp"));
        }
        auto begun = backend_->beginTransaction();
        if(!begun) {
            return foundation::Result<void>::failure(std::move(begun).error());
        }
        transactionOpen = true;
        const std::array queryParameters {
            foundation::Value {std::string(snapshot.taskId.value())}};
        auto rows = backend_->query(
            "SELECT task_name,status,terminal_payload,terminal_digest "
            "FROM task_history WHERE task_id=?",
            queryParameters);
        if(!rows) {
            return rollback(*backend_, std::move(rows).error());
        }
        if(rows.value().empty()) {
            return rollback(*backend_, taskPersistenceError(
                "Persistence.TaskHistoryMissing",
                foundation::ErrorCategory::NotFound,
                "A terminal task has no durable acceptance record"));
        }
        if(rows.value().size() != 1U) {
            return rollback(*backend_, taskPersistenceError(
                "Persistence.InvalidTaskRow",
                foundation::ErrorCategory::Infrastructure,
                "A task identity resolved ambiguously"));
        }
        const auto& row = rows.value().front();
        auto taskName = idColumn<kernel::TaskName>(row, "task_name");
        auto status = textColumn(row, "status");
        if(!taskName || !status || taskName.value() != snapshot.task) {
            return rollback(*backend_, taskPersistenceError(
                "Persistence.TaskIdentityConflict",
                foundation::ErrorCategory::Conflict,
                "A task outcome does not match its durable acceptance identity"));
        }
        if(status.value() == "succeeded" || status.value() == "failed"
           || status.value() == "cancelled" || status.value() == "stale") {
            auto storedPayload = textColumn(row, "terminal_payload");
            auto storedDigest = idColumn<kernel::ContentDigest>(row, "terminal_digest");
            if(!storedPayload || !storedDigest
               || status.value() != stateName(snapshot.state)
               || storedPayload.value() != payload.value()
               || storedDigest.value() != digest.value()) {
                return rollback(*backend_, taskPersistenceError(
                    "Persistence.TaskOutcomeConflict",
                    foundation::ErrorCategory::Conflict,
                    "A durable task already has a different terminal outcome"));
            }
            auto committed = backend_->commitTransaction();
            if(!committed) {
                return rollback(*backend_, std::move(committed).error());
            }
            transactionOpen = false;
            return foundation::Result<void>::success();
        }
        if(status.value() != "accepted" && status.value() != "running") {
            return rollback(*backend_, taskPersistenceError(
                "Persistence.TaskOutcomeConflict",
                foundation::ErrorCategory::Conflict,
                "A durable task is no longer eligible for terminal completion"));
        }
        const std::array parameters {
            foundation::Value {stateName(snapshot.state)},
            foundation::Value {payload.value()},
            foundation::Value {std::string(digest.value().value())},
            foundation::Value {now},
            foundation::Value {std::string(snapshot.taskId.value())}};
        auto updated = backend_->execute(
            "UPDATE task_history SET status=?,terminal_payload=?,terminal_digest=?,"
            "updated_at_ms=? WHERE task_id=? AND status IN ('accepted','running')",
            parameters);
        if(!updated || updated.value() != 1U) {
            auto error = updated
                ? taskPersistenceError(
                      "Persistence.TaskOutcomeConflict",
                      foundation::ErrorCategory::Conflict,
                      "The durable task changed before terminal completion")
                : std::move(updated).error();
            return rollback(*backend_, std::move(error));
        }
        auto committed = backend_->commitTransaction();
        if(!committed) {
            return rollback(*backend_, std::move(committed).error());
        }
        transactionOpen = false;
        return foundation::Result<void>::success();
    } catch(const std::exception& exception) {
        auto error = taskPersistenceError(
            "Persistence.TaskOutcomeFailed",
            foundation::ErrorCategory::Internal,
            "Durable task completion failed unexpectedly",
            {{"reason", foundation::Value {exception.what()}}});
        return transactionOpen ? rollback(*backend_, std::move(error))
                               : foundation::Result<void>::failure(std::move(error));
    } catch(...) {
        auto error = taskPersistenceError(
            "Persistence.TaskOutcomeFailed",
            foundation::ErrorCategory::Internal,
            "Durable task completion failed unexpectedly");
        return transactionOpen ? rollback(*backend_, std::move(error))
                               : foundation::Result<void>::failure(std::move(error));
    }
}

foundation::Result<std::optional<runtime::TaskSnapshot>> PersistenceService::taskHistory(
    const kernel::TaskId& taskId) const
{
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<std::optional<runtime::TaskSnapshot>>::failure(
            taskPersistenceError(
                "Persistence.NotReady",
                foundation::ErrorCategory::Conflict,
                "Persistence must be initialized before reading task history"));
    }
    const std::array parameters {foundation::Value {std::string(taskId.value())}};
    auto rows = backend_->query(
        "SELECT task_name,status,request_payload,request_digest,terminal_payload,"
        "terminal_digest FROM task_history WHERE task_id=?",
        parameters);
    if(!rows) {
        return foundation::Result<std::optional<runtime::TaskSnapshot>>::failure(
            std::move(rows).error());
    }
    if(rows.value().empty()) {
        return foundation::Result<std::optional<runtime::TaskSnapshot>>::success(
            std::nullopt);
    }
    if(rows.value().size() != 1U) {
        return foundation::Result<std::optional<runtime::TaskSnapshot>>::failure(
            taskPersistenceError(
                "Persistence.InvalidTaskRow",
                foundation::ErrorCategory::Infrastructure,
                "A task history identity resolved ambiguously"));
    }
    const auto& row = rows.value().front();
    auto taskName = idColumn<kernel::TaskName>(row, "task_name");
    auto status = textColumn(row, "status");
    auto requestPayload = textColumn(row, "request_payload");
    auto requestDigest = idColumn<kernel::ContentDigest>(row, "request_digest");
    if(!taskName || !status || !requestPayload || !requestDigest) {
        return foundation::Result<std::optional<runtime::TaskSnapshot>>::failure(
            taskPersistenceError(
                "Persistence.InvalidTaskRow",
                foundation::ErrorCategory::Infrastructure,
                "A task history row is incomplete"));
    }
    auto requestValid = validateDigest(
        requestPayload.value(),
        requestDigest.value(),
        *hashes_,
        "Persistence.TaskRequestDigestMismatch");
    if(!requestValid) {
        return foundation::Result<std::optional<runtime::TaskSnapshot>>::failure(
            std::move(requestValid).error());
    }
    auto request = serializer_->deserialize(requestPayload.value());
    if(!request) {
        return foundation::Result<std::optional<runtime::TaskSnapshot>>::failure(
            std::move(request).error());
    }
    const auto* requestRoot = request.value().getIf<foundation::Value::Object>();
    const auto* traceValue = requestRoot == nullptr
        ? nullptr
        : field(*requestRoot, "traceId");
    const auto* traceText = traceValue == nullptr
        ? nullptr
        : traceValue->getIf<std::string>();
    const auto* sourceValue = requestRoot == nullptr
        ? nullptr
        : field(*requestRoot, "sourceRevisions");
    const auto* indexedTaskValue = requestRoot == nullptr
        ? nullptr
        : field(*requestRoot, "taskId");
    const auto* indexedTaskText = indexedTaskValue == nullptr
        ? nullptr
        : indexedTaskValue->getIf<std::string>();
    const auto* indexedNameValue = requestRoot == nullptr
        ? nullptr
        : field(*requestRoot, "task");
    const auto* indexedNameText = indexedNameValue == nullptr
        ? nullptr
        : indexedNameValue->getIf<std::string>();
    if(traceText == nullptr || sourceValue == nullptr || indexedTaskText == nullptr
       || *indexedTaskText != taskId.value() || indexedNameText == nullptr
       || *indexedNameText != taskName.value().value()) {
        return foundation::Result<std::optional<runtime::TaskSnapshot>>::failure(
            taskPersistenceError(
                "Persistence.InvalidTaskPayload",
                foundation::ErrorCategory::Infrastructure,
                "Task acceptance payload metadata does not match its index"));
    }
    auto traceId = idText<kernel::TraceId>(*traceText, "traceId");
    auto sourceRevisions = optionalRevisionValue(*sourceValue);
    if(!traceId || !sourceRevisions) {
        return foundation::Result<std::optional<runtime::TaskSnapshot>>::failure(
            taskPersistenceError(
                "Persistence.InvalidTaskPayload",
                foundation::ErrorCategory::Infrastructure,
                "Task acceptance payload contains invalid typed fields"));
    }
    if(status.value() == "accepted" || status.value() == "running") {
        return foundation::Result<std::optional<runtime::TaskSnapshot>>::success(
            runtime::TaskSnapshot {
                taskId,
                taskName.value(),
                runtime::TaskState::Pending,
                0.0,
                {},
                std::move(traceId).value(),
                std::move(sourceRevisions).value(),
                std::nullopt,
                std::nullopt});
    }
    if(status.value() == "interrupted") {
        return foundation::Result<std::optional<runtime::TaskSnapshot>>::success(
            runtime::TaskSnapshot {
                taskId,
                taskName.value(),
                runtime::TaskState::Failed,
                0.0,
                {},
                std::move(traceId).value(),
                std::move(sourceRevisions).value(),
                std::nullopt,
                foundation::makeError(
                    "Task.InterruptedByRestart",
                    foundation::ErrorCategory::Infrastructure,
                    "The task did not reach a durable terminal state before restart")});
    }
    auto terminalPayload = textColumn(row, "terminal_payload");
    auto terminalDigest = idColumn<kernel::ContentDigest>(row, "terminal_digest");
    if(!terminalPayload || !terminalDigest) {
        return foundation::Result<std::optional<runtime::TaskSnapshot>>::failure(
            taskPersistenceError(
                "Persistence.InvalidTaskRow",
                foundation::ErrorCategory::Infrastructure,
                "A terminal task history row is incomplete"));
    }
    auto terminalValid = validateDigest(
        terminalPayload.value(),
        terminalDigest.value(),
        *hashes_,
        "Persistence.TaskTerminalDigestMismatch");
    if(!terminalValid) {
        return foundation::Result<std::optional<runtime::TaskSnapshot>>::failure(
            std::move(terminalValid).error());
    }
    auto decodedTerminal = decodeTerminal(
        terminalPayload.value(), taskId, taskName.value(), *serializer_);
    if(!decodedTerminal || stateName(decodedTerminal.value().state) != status.value()) {
        return foundation::Result<std::optional<runtime::TaskSnapshot>>::failure(
            taskPersistenceError(
                "Persistence.TaskTerminalMismatch",
                foundation::ErrorCategory::Infrastructure,
                "A terminal task payload does not match its control-plane status"));
    }
    return foundation::Result<std::optional<runtime::TaskSnapshot>>::success(
        std::move(decodedTerminal).value());
}

} // namespace lasercnc::persistence
