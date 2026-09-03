#include <lasercnc/persistence/persistence_service.hpp>

#include <lasercnc/foundation/error.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lasercnc::persistence {
namespace {

foundation::Error workflowPersistenceError(
    std::string code,
    foundation::ErrorCategory category,
    std::string message,
    foundation::Value::Object details = {},
    std::shared_ptr<const foundation::Error> cause = nullptr)
{
    return foundation::makeError(
        std::move(code),
        category,
        std::move(message),
        foundation::Value {std::move(details)},
        foundation::Severity::Error,
        std::move(cause));
}

std::span<const std::byte> bytes(std::string_view value) noexcept
{
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
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
    return foundation::Result<void>::failure(workflowPersistenceError(
        "Persistence.RollbackFailed",
        foundation::ErrorCategory::Infrastructure,
        "Workflow persistence failed and could not roll back",
        {{"rollbackCode", foundation::Value {std::string(rollbackError.code.value())}},
         {"rollbackMessage", foundation::Value {rollbackError.message}}},
        std::make_shared<const foundation::Error>(std::move(primary))));
}

foundation::Value versionValue(const foundation::Version& version)
{
    return foundation::Value {foundation::Value::Object {
        {"major", foundation::Value {static_cast<std::int64_t>(version.major)}},
        {"minor", foundation::Value {static_cast<std::int64_t>(version.minor)}},
        {"patch", foundation::Value {static_cast<std::int64_t>(version.patch)}},
    }};
}

foundation::Value schemaValue(const foundation::Schema& schema)
{
    return foundation::Value {foundation::Value::Object {
        {"constraints", schema.constraints()},
        {"id", foundation::Value {std::string(schema.id().value())}},
        {"kind", foundation::Value {static_cast<std::int64_t>(schema.rootKind())}},
        {"unit", schema.unit().has_value()
            ? foundation::Value {*schema.unit()}
            : foundation::Value {}},
        {"version", versionValue(schema.version())},
    }};
}

foundation::Value optionalPredicateValue(
    const std::optional<runtime::WorkflowPredicate>& predicate)
{
    if(!predicate.has_value()) {
        return foundation::Value {};
    }
    return foundation::Value {foundation::Value::Object {
        {"expected", predicate->expected},
        {"kind", foundation::Value {static_cast<std::int64_t>(predicate->kind)}},
        {"variablePath", foundation::Value {predicate->variablePath}},
    }};
}

foundation::Value commandCallValue(const runtime::WorkflowCommandCall& call)
{
    return foundation::Value {foundation::Value::Object {
        {"argumentsTemplate", call.argumentsTemplate},
        {"command", foundation::Value {std::string(call.command.value())}},
        {"version", versionValue(call.version)},
    }};
}

foundation::Value optionalCommandValue(
    const std::optional<runtime::WorkflowCommandCall>& call)
{
    return call.has_value() ? commandCallValue(*call) : foundation::Value {};
}

foundation::Value optionalQueryValue(
    const std::optional<runtime::WorkflowQueryCall>& call)
{
    if(!call.has_value()) {
        return foundation::Value {};
    }
    return foundation::Value {foundation::Value::Object {
        {"argumentsTemplate", call->argumentsTemplate},
        {"query", foundation::Value {std::string(call->query.value())}},
        {"version", versionValue(call->version)},
    }};
}

foundation::Value definitionValue(const runtime::WorkflowDefinition& definition)
{
    foundation::Value::Array steps;
    steps.reserve(definition.steps.size());
    for(const auto& step : definition.steps) {
        foundation::Value::Array dependencies;
        dependencies.reserve(step.dependencies.size());
        for(const auto& dependency : step.dependencies) {
            dependencies.emplace_back(std::string(dependency.value()));
        }
        foundation::Value::Array retryCodes;
        retryCodes.reserve(step.retry.retryableErrorCodes.size());
        for(const auto& code : step.retry.retryableErrorCodes) {
            retryCodes.emplace_back(code);
        }
        steps.emplace_back(foundation::Value::Object {
            {"command", optionalCommandValue(step.command)},
            {"compensation", step.compensation.has_value()
                ? commandCallValue(step.compensation->command)
                : foundation::Value {}},
            {"condition", optionalPredicateValue(step.condition)},
            {"dependencies", foundation::Value {std::move(dependencies)}},
            {"kind", foundation::Value {static_cast<std::int64_t>(step.kind)}},
            {"query", optionalQueryValue(step.query)},
            {"resultBinding", foundation::Value {step.resultBinding}},
            {"retry", foundation::Value {foundation::Value::Object {
                {"backoffMs", foundation::Value {
                    static_cast<std::int64_t>(step.retry.backoff.count())}},
                {"maxAttempts", foundation::Value {
                    static_cast<std::int64_t>(step.retry.maxAttempts)}},
                {"retryableErrorCodes", foundation::Value {std::move(retryCodes)}},
            }}},
            {"stepId", foundation::Value {std::string(step.stepId.value())}},
            {"taskIdBinding", foundation::Value {step.taskIdBinding}},
            {"taskIdVariablePath", foundation::Value {step.taskIdVariablePath}},
            {"timeoutMs", step.timeout.has_value()
                ? foundation::Value {static_cast<std::int64_t>(step.timeout->count())}
                : foundation::Value {}},
            {"valueTemplate", step.valueTemplate},
        });
    }
    return foundation::Value {foundation::Value::Object {
        {"format", foundation::Value {"lasercnc.workflow-definition"}},
        {"inputSchema", schemaValue(definition.descriptor.input)},
        {"name", foundation::Value {std::string(definition.descriptor.name.value())}},
        {"resultSchema", schemaValue(definition.descriptor.result)},
        {"resultTemplate", definition.resultTemplate},
        {"steps", foundation::Value {std::move(steps)}},
        {"version", versionValue(definition.descriptor.version)},
        {"wireVersion", foundation::Value {std::int64_t {1}}},
    }};
}

const char* workflowStateName(runtime::WorkflowState state) noexcept
{
    switch(state) {
    case runtime::WorkflowState::Pending: return "pending";
    case runtime::WorkflowState::Running: return "running";
    case runtime::WorkflowState::Waiting: return "waiting";
    case runtime::WorkflowState::Succeeded: return "succeeded";
    case runtime::WorkflowState::Failed: return "failed";
    case runtime::WorkflowState::CancelRequested: return "cancel_requested";
    case runtime::WorkflowState::Compensating: return "compensating";
    case runtime::WorkflowState::Cancelled: return "cancelled";
    case runtime::WorkflowState::Compensated: return "compensated";
    case runtime::WorkflowState::CompensationFailed: return "compensation_failed";
    }
    return "unknown";
}

const char* stepStateName(runtime::WorkflowStepState state) noexcept
{
    switch(state) {
    case runtime::WorkflowStepState::Pending: return "pending";
    case runtime::WorkflowStepState::Ready: return "ready";
    case runtime::WorkflowStepState::Running: return "running";
    case runtime::WorkflowStepState::Waiting: return "waiting";
    case runtime::WorkflowStepState::Succeeded: return "succeeded";
    case runtime::WorkflowStepState::Skipped: return "skipped";
    case runtime::WorkflowStepState::Failed: return "failed";
    case runtime::WorkflowStepState::Cancelled: return "cancelled";
    case runtime::WorkflowStepState::Compensated: return "compensated";
    case runtime::WorkflowStepState::CompensationFailed: return "compensation_failed";
    }
    return "unknown";
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

foundation::Value optionalTimeValue(
    const std::optional<std::chrono::system_clock::time_point>& value)
{
    if(!value.has_value()) {
        return foundation::Value {};
    }
    return foundation::Value {std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            value->time_since_epoch()).count())};
}

foundation::Value stepSnapshotValue(const runtime::WorkflowStepSnapshot& step)
{
    return foundation::Value {foundation::Value::Object {
        {"attempt", foundation::Value {static_cast<std::int64_t>(step.attempt)}},
        {"compensationAttempt", foundation::Value {
            static_cast<std::int64_t>(step.compensationAttempt)}},
        {"error", errorValue(step.error)},
        {"format", foundation::Value {"lasercnc.workflow-step-checkpoint"}},
        {"nextAttemptAtMs", optionalTimeValue(step.nextAttemptAt)},
        {"result", step.result.has_value() ? *step.result : foundation::Value {}},
        {"replayCompensationAttempt", foundation::Value {
            step.replayCompensationAttempt}},
        {"replayCurrentAttempt", foundation::Value {step.replayCurrentAttempt}},
        {"startedAtMs", optionalTimeValue(step.startedAt)},
        {"state", foundation::Value {stepStateName(step.state)}},
        {"stepId", foundation::Value {std::string(step.stepId.value())}},
        {"taskId", step.taskId.has_value()
            ? foundation::Value {std::string(step.taskId->value())}
            : foundation::Value {}},
        {"version", foundation::Value {std::int64_t {1}}},
    }};
}

foundation::Value checkpointValue(
    const runtime::WorkflowRequest& request,
    const kernel::ContentDigest& definitionDigest,
    const runtime::WorkflowSnapshot& snapshot,
    const std::vector<kernel::WorkflowStepId>& completionOrder)
{
    foundation::Value::Array steps;
    steps.reserve(snapshot.steps.size());
    for(const auto& step : snapshot.steps) {
        steps.push_back(stepSnapshotValue(step));
    }
    foundation::Value::Array completed;
    completed.reserve(completionOrder.size());
    for(const auto& stepId : completionOrder) {
        completed.emplace_back(std::string(stepId.value()));
    }
    foundation::Value::Array compensationErrors;
    compensationErrors.reserve(snapshot.compensationErrors.size());
    for(const auto& error : snapshot.compensationErrors) {
        compensationErrors.push_back(errorValue(error));
    }
    return foundation::Value {foundation::Value::Object {
        {"completionOrder", foundation::Value {std::move(completed)}},
        {"compensationErrors", foundation::Value {std::move(compensationErrors)}},
        {"correlationId", foundation::Value {std::string(request.correlationId.value())}},
        {"deadlineMs", optionalTimeValue(request.deadline)},
        {"definitionDigest", foundation::Value {std::string(definitionDigest.value())}},
        {"documentId", foundation::Value {std::string(request.documentId.value())}},
        {"error", errorValue(snapshot.error)},
        {"format", foundation::Value {"lasercnc.workflow-checkpoint"}},
        {"input", request.input},
        {"parentSpanId", request.parentSpanId.has_value()
            ? foundation::Value {std::string(request.parentSpanId->value())}
            : foundation::Value {}},
        {"projectId", foundation::Value {std::string(request.projectId.value())}},
        {"result", snapshot.result.has_value() ? *snapshot.result : foundation::Value {}},
        {"sessionId", foundation::Value {std::string(request.sessionId.value())}},
        {"state", foundation::Value {workflowStateName(snapshot.state)}},
        {"steps", foundation::Value {std::move(steps)}},
        {"traceId", foundation::Value {std::string(request.traceId.value())}},
        {"variables", snapshot.variables},
        {"version", versionValue(snapshot.version)},
        {"wireVersion", foundation::Value {std::int64_t {1}}},
        {"workflow", foundation::Value {std::string(snapshot.workflow.value())}},
        {"workflowId", foundation::Value {std::string(snapshot.workflowId.value())}},
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
        return foundation::Result<std::string>::failure(workflowPersistenceError(
            "Persistence.InvalidWorkflowRow",
            foundation::ErrorCategory::Infrastructure,
            "A workflow row contains an invalid text column",
            {{"column", foundation::Value {name}}}));
    }
    return foundation::Result<std::string>::success(*value);
}

foundation::Result<std::int64_t> integerColumn(
    const platform::PersistenceRow& row,
    const char* name)
{
    const auto found = row.find(name);
    const auto* value = found == row.end() ? nullptr : found->second.getIf<std::int64_t>();
    if(value == nullptr) {
        return foundation::Result<std::int64_t>::failure(workflowPersistenceError(
            "Persistence.InvalidWorkflowRow",
            foundation::ErrorCategory::Infrastructure,
            "A workflow row contains an invalid integer column",
            {{"column", foundation::Value {name}}}));
    }
    return foundation::Result<std::int64_t>::success(*value);
}

template <typename Id>
foundation::Result<Id> idText(std::string text, const char* name)
{
    auto id = Id::create(std::move(text));
    if(!id) {
        return foundation::Result<Id>::failure(workflowPersistenceError(
            "Persistence.InvalidWorkflowIdentity",
            foundation::ErrorCategory::Infrastructure,
            "A workflow checkpoint contains an invalid stable identity",
            {{"field", foundation::Value {name}}},
            std::make_shared<const foundation::Error>(std::move(id).error())));
    }
    return id;
}

template <typename Id>
foundation::Result<Id> idColumn(const platform::PersistenceRow& row, const char* name)
{
    auto text = textColumn(row, name);
    if(!text) {
        return foundation::Result<Id>::failure(std::move(text).error());
    }
    return idText<Id>(std::move(text).value(), name);
}

foundation::Result<foundation::Version> parseVersion(const foundation::Value& value)
{
    const auto* object = value.getIf<foundation::Value::Object>();
    const auto* majorValue = object == nullptr ? nullptr : field(*object, "major");
    const auto* minorValue = object == nullptr ? nullptr : field(*object, "minor");
    const auto* patchValue = object == nullptr ? nullptr : field(*object, "patch");
    const auto* major = majorValue == nullptr ? nullptr : majorValue->getIf<std::int64_t>();
    const auto* minor = minorValue == nullptr ? nullptr : minorValue->getIf<std::int64_t>();
    const auto* patch = patchValue == nullptr ? nullptr : patchValue->getIf<std::int64_t>();
    constexpr auto maximum = static_cast<std::int64_t>(
        std::numeric_limits<std::uint32_t>::max());
    if(major == nullptr || minor == nullptr || patch == nullptr || *major < 0
       || *minor < 0 || *patch < 0 || *major > maximum || *minor > maximum
       || *patch > maximum) {
        return foundation::Result<foundation::Version>::failure(workflowPersistenceError(
            "Persistence.InvalidWorkflowPayload",
            foundation::ErrorCategory::Infrastructure,
            "A workflow version is invalid"));
    }
    return foundation::Result<foundation::Version>::success(foundation::Version {
        static_cast<std::uint32_t>(*major),
        static_cast<std::uint32_t>(*minor),
        static_cast<std::uint32_t>(*patch)});
}

foundation::Result<std::optional<std::chrono::system_clock::time_point>> parseOptionalTime(
    const foundation::Value& value)
{
    if(value.kind() == foundation::Value::Kind::Null) {
        return foundation::Result<std::optional<std::chrono::system_clock::time_point>>::success(
            std::nullopt);
    }
    const auto* text = value.getIf<std::string>();
    if(text == nullptr) {
        return foundation::Result<std::optional<std::chrono::system_clock::time_point>>::failure(
            workflowPersistenceError(
                "Persistence.InvalidWorkflowPayload",
                foundation::ErrorCategory::Infrastructure,
                "A workflow timestamp must be a decimal string"));
    }
    std::int64_t milliseconds = 0;
    const auto parsed = std::from_chars(
        text->data(), text->data() + text->size(), milliseconds);
    if(parsed.ec != std::errc {} || parsed.ptr != text->data() + text->size()
       || milliseconds < 0) {
        return foundation::Result<std::optional<std::chrono::system_clock::time_point>>::failure(
            workflowPersistenceError(
                "Persistence.InvalidWorkflowPayload",
                foundation::ErrorCategory::Infrastructure,
                "A workflow timestamp is invalid"));
    }
    return foundation::Result<std::optional<std::chrono::system_clock::time_point>>::success(
        std::chrono::system_clock::time_point {std::chrono::milliseconds {milliseconds}});
}

foundation::Result<std::optional<foundation::Error>> parseError(
    const foundation::Value& value)
{
    if(value.kind() == foundation::Value::Kind::Null) {
        return foundation::Result<std::optional<foundation::Error>>::success(std::nullopt);
    }
    const auto* object = value.getIf<foundation::Value::Object>();
    const auto* codeValue = object == nullptr ? nullptr : field(*object, "code");
    const auto* messageValue = object == nullptr ? nullptr : field(*object, "message");
    const auto* categoryValue = object == nullptr ? nullptr : field(*object, "category");
    const auto* severityValue = object == nullptr ? nullptr : field(*object, "severity");
    const auto* details = object == nullptr ? nullptr : field(*object, "details");
    const auto* code = codeValue == nullptr ? nullptr : codeValue->getIf<std::string>();
    const auto* message = messageValue == nullptr ? nullptr : messageValue->getIf<std::string>();
    const auto* category = categoryValue == nullptr
        ? nullptr
        : categoryValue->getIf<std::int64_t>();
    const auto* severity = severityValue == nullptr
        ? nullptr
        : severityValue->getIf<std::int64_t>();
    if(code == nullptr || code->empty() || message == nullptr || category == nullptr
       || severity == nullptr || details == nullptr || *category < 0
       || *category > static_cast<std::int64_t>(foundation::ErrorCategory::Internal)
       || *severity < 0
       || *severity > static_cast<std::int64_t>(foundation::Severity::Fatal)) {
        return foundation::Result<std::optional<foundation::Error>>::failure(
            workflowPersistenceError(
                "Persistence.InvalidWorkflowPayload",
                foundation::ErrorCategory::Infrastructure,
                "A workflow error payload is invalid"));
    }
    return foundation::Result<std::optional<foundation::Error>>::success(
        foundation::makeError(
            *code,
            static_cast<foundation::ErrorCategory>(*category),
            *message,
            *details,
            static_cast<foundation::Severity>(*severity)));
}

foundation::Result<runtime::WorkflowState> parseWorkflowState(std::string_view value)
{
    constexpr std::array states {
        runtime::WorkflowState::Pending,
        runtime::WorkflowState::Running,
        runtime::WorkflowState::Waiting,
        runtime::WorkflowState::Succeeded,
        runtime::WorkflowState::Failed,
        runtime::WorkflowState::CancelRequested,
        runtime::WorkflowState::Compensating,
        runtime::WorkflowState::Cancelled,
        runtime::WorkflowState::Compensated,
        runtime::WorkflowState::CompensationFailed};
    const auto found = std::ranges::find_if(
        states, [&](auto state) { return value == workflowStateName(state); });
    if(found == states.end()) {
        return foundation::Result<runtime::WorkflowState>::failure(workflowPersistenceError(
            "Persistence.InvalidWorkflowState",
            foundation::ErrorCategory::Infrastructure,
            "A workflow checkpoint state is invalid"));
    }
    return foundation::Result<runtime::WorkflowState>::success(*found);
}

foundation::Result<runtime::WorkflowStepState> parseStepState(std::string_view value)
{
    constexpr std::array states {
        runtime::WorkflowStepState::Pending,
        runtime::WorkflowStepState::Ready,
        runtime::WorkflowStepState::Running,
        runtime::WorkflowStepState::Waiting,
        runtime::WorkflowStepState::Succeeded,
        runtime::WorkflowStepState::Skipped,
        runtime::WorkflowStepState::Failed,
        runtime::WorkflowStepState::Cancelled,
        runtime::WorkflowStepState::Compensated,
        runtime::WorkflowStepState::CompensationFailed};
    const auto found = std::ranges::find_if(
        states, [&](auto state) { return value == stepStateName(state); });
    if(found == states.end()) {
        return foundation::Result<runtime::WorkflowStepState>::failure(
            workflowPersistenceError(
                "Persistence.InvalidWorkflowStepState",
                foundation::ErrorCategory::Infrastructure,
                "A workflow step checkpoint state is invalid"));
    }
    return foundation::Result<runtime::WorkflowStepState>::success(*found);
}

foundation::Result<runtime::WorkflowStepSnapshot> parseStepSnapshot(
    const foundation::Value& value)
{
    const auto* object = value.getIf<foundation::Value::Object>();
    const auto* formatValue = object == nullptr ? nullptr : field(*object, "format");
    const auto* versionValue = object == nullptr ? nullptr : field(*object, "version");
    const auto* stepIdValue = object == nullptr ? nullptr : field(*object, "stepId");
    const auto* stateValue = object == nullptr ? nullptr : field(*object, "state");
    const auto* attemptValue = object == nullptr ? nullptr : field(*object, "attempt");
    const auto* compensationValue = object == nullptr
        ? nullptr
        : field(*object, "compensationAttempt");
    const auto* nextAttempt = object == nullptr ? nullptr : field(*object, "nextAttemptAtMs");
    const auto* started = object == nullptr ? nullptr : field(*object, "startedAtMs");
    const auto* taskIdValue = object == nullptr ? nullptr : field(*object, "taskId");
    const auto* resultValue = object == nullptr ? nullptr : field(*object, "result");
    const auto* replayCurrentValue = object == nullptr
        ? nullptr
        : field(*object, "replayCurrentAttempt");
    const auto* replayCompensationValue = object == nullptr
        ? nullptr
        : field(*object, "replayCompensationAttempt");
    const auto* errorPayload = object == nullptr ? nullptr : field(*object, "error");
    const auto* format = formatValue == nullptr ? nullptr : formatValue->getIf<std::string>();
    const auto* wireVersion = versionValue == nullptr
        ? nullptr
        : versionValue->getIf<std::int64_t>();
    const auto* stepIdText = stepIdValue == nullptr
        ? nullptr
        : stepIdValue->getIf<std::string>();
    const auto* stateText = stateValue == nullptr ? nullptr : stateValue->getIf<std::string>();
    const auto* attempt = attemptValue == nullptr
        ? nullptr
        : attemptValue->getIf<std::int64_t>();
    const auto* compensationAttempt = compensationValue == nullptr
        ? nullptr
        : compensationValue->getIf<std::int64_t>();
    const auto* replayCurrent = replayCurrentValue == nullptr
        ? nullptr
        : replayCurrentValue->getIf<bool>();
    const auto* replayCompensation = replayCompensationValue == nullptr
        ? nullptr
        : replayCompensationValue->getIf<bool>();
    constexpr auto maximumAttempt = static_cast<std::int64_t>(
        std::numeric_limits<std::uint32_t>::max());
    if(format == nullptr || *format != "lasercnc.workflow-step-checkpoint"
       || wireVersion == nullptr || *wireVersion != 1 || stepIdText == nullptr
       || stateText == nullptr || attempt == nullptr || compensationAttempt == nullptr
       || replayCurrent == nullptr || replayCompensation == nullptr
       || nextAttempt == nullptr || started == nullptr || taskIdValue == nullptr
       || resultValue == nullptr || errorPayload == nullptr || *attempt < 0
       || *compensationAttempt < 0
       || *attempt > maximumAttempt || *compensationAttempt > maximumAttempt) {
        return foundation::Result<runtime::WorkflowStepSnapshot>::failure(
            workflowPersistenceError(
                "Persistence.InvalidWorkflowStepPayload",
                foundation::ErrorCategory::Infrastructure,
                "A workflow step checkpoint is incomplete"));
    }
    auto stepId = idText<kernel::WorkflowStepId>(*stepIdText, "stepId");
    auto state = parseStepState(*stateText);
    auto nextAttemptAt = parseOptionalTime(*nextAttempt);
    auto startedAt = parseOptionalTime(*started);
    auto error = parseError(*errorPayload);
    if(!stepId || !state || !nextAttemptAt || !startedAt || !error) {
        return foundation::Result<runtime::WorkflowStepSnapshot>::failure(
            workflowPersistenceError(
                "Persistence.InvalidWorkflowStepPayload",
                foundation::ErrorCategory::Infrastructure,
                "A workflow step checkpoint has invalid typed fields"));
    }
    std::optional<kernel::TaskId> taskId;
    if(taskIdValue->kind() != foundation::Value::Kind::Null) {
        const auto* text = taskIdValue->getIf<std::string>();
        if(text == nullptr) {
            return foundation::Result<runtime::WorkflowStepSnapshot>::failure(
                workflowPersistenceError(
                    "Persistence.InvalidWorkflowStepPayload",
                    foundation::ErrorCategory::Infrastructure,
                    "A workflow step TaskId is invalid"));
        }
        auto parsed = idText<kernel::TaskId>(*text, "taskId");
        if(!parsed) {
            return foundation::Result<runtime::WorkflowStepSnapshot>::failure(
                std::move(parsed).error());
        }
        taskId = std::move(parsed).value();
    }
    std::optional<foundation::Value> result;
    if(resultValue->kind() != foundation::Value::Kind::Null) {
        result = *resultValue;
    }
    return foundation::Result<runtime::WorkflowStepSnapshot>::success(
        runtime::WorkflowStepSnapshot {
            std::move(stepId).value(),
            state.value(),
            static_cast<std::uint32_t>(*attempt),
            std::move(nextAttemptAt).value(),
            std::move(startedAt).value(),
            std::move(taskId),
            std::move(result),
            std::move(error).value(),
            static_cast<std::uint32_t>(*compensationAttempt),
            *replayCurrent,
            *replayCompensation});
}

foundation::Result<void> validateDigest(
    std::string_view payload,
    const kernel::ContentDigest& expected,
    const platform::IHashService& hashes,
    const char* code)
{
    auto actual = hashes.digest(bytes(payload));
    if(!actual) {
        return foundation::Result<void>::failure(std::move(actual).error());
    }
    if(actual.value() != expected) {
        return foundation::Result<void>::failure(workflowPersistenceError(
            code,
            foundation::ErrorCategory::Infrastructure,
            "A workflow checkpoint failed its content digest check"));
    }
    return foundation::Result<void>::success();
}

foundation::Result<WorkflowCheckpoint> decodeCheckpoint(
    const platform::PersistenceRow& row,
    const foundation::IValueSerializer& serializer,
    const platform::IHashService& hashes)
{
    auto indexedId = idColumn<kernel::WorkflowId>(row, "workflow_id");
    auto indexedName = idColumn<kernel::WorkflowName>(row, "workflow_name");
    auto major = integerColumn(row, "workflow_major");
    auto minor = integerColumn(row, "workflow_minor");
    auto patch = integerColumn(row, "workflow_patch");
    auto indexedDefinitionDigest = idColumn<kernel::ContentDigest>(row, "definition_digest");
    auto indexedState = textColumn(row, "status");
    auto payload = textColumn(row, "payload");
    auto digest = idColumn<kernel::ContentDigest>(row, "digest");
    auto updatedAt = integerColumn(row, "updated_at_ms");
    constexpr auto maximumVersion = static_cast<std::int64_t>(
        std::numeric_limits<std::uint32_t>::max());
    if(!indexedId || !indexedName || !major || !minor || !patch
       || !indexedDefinitionDigest || !indexedState || !payload || !digest || !updatedAt
       || major.value() < 0 || minor.value() < 0 || patch.value() < 0
       || major.value() > maximumVersion || minor.value() > maximumVersion
       || patch.value() > maximumVersion
       || updatedAt.value() < 0) {
        return foundation::Result<WorkflowCheckpoint>::failure(workflowPersistenceError(
            "Persistence.InvalidWorkflowRow",
            foundation::ErrorCategory::Infrastructure,
            "A workflow instance row is invalid"));
    }
    auto validDigest = validateDigest(
        payload.value(), digest.value(), hashes, "Persistence.WorkflowDigestMismatch");
    if(!validDigest) {
        return foundation::Result<WorkflowCheckpoint>::failure(
            std::move(validDigest).error());
    }
    auto decoded = serializer.deserialize(payload.value());
    if(!decoded) {
        return foundation::Result<WorkflowCheckpoint>::failure(std::move(decoded).error());
    }
    const auto* root = decoded.value().getIf<foundation::Value::Object>();
    const auto* formatValue = root == nullptr ? nullptr : field(*root, "format");
    const auto* wireVersionValue = root == nullptr ? nullptr : field(*root, "wireVersion");
    const auto* workflowIdValue = root == nullptr ? nullptr : field(*root, "workflowId");
    const auto* workflowValue = root == nullptr ? nullptr : field(*root, "workflow");
    const auto* version = root == nullptr ? nullptr : field(*root, "version");
    const auto* definitionDigestValue = root == nullptr
        ? nullptr
        : field(*root, "definitionDigest");
    const auto* stateValue = root == nullptr ? nullptr : field(*root, "state");
    const auto* input = root == nullptr ? nullptr : field(*root, "input");
    const auto* variables = root == nullptr ? nullptr : field(*root, "variables");
    const auto* sessionValue = root == nullptr ? nullptr : field(*root, "sessionId");
    const auto* projectValue = root == nullptr ? nullptr : field(*root, "projectId");
    const auto* documentValue = root == nullptr ? nullptr : field(*root, "documentId");
    const auto* correlationValue = root == nullptr ? nullptr : field(*root, "correlationId");
    const auto* traceValue = root == nullptr ? nullptr : field(*root, "traceId");
    const auto* parentSpanValue = root == nullptr ? nullptr : field(*root, "parentSpanId");
    const auto* deadlineValue = root == nullptr ? nullptr : field(*root, "deadlineMs");
    const auto* stepsValue = root == nullptr ? nullptr : field(*root, "steps");
    const auto* resultValue = root == nullptr ? nullptr : field(*root, "result");
    const auto* errorPayload = root == nullptr ? nullptr : field(*root, "error");
    const auto* compensationErrorsValue = root == nullptr
        ? nullptr
        : field(*root, "compensationErrors");
    const auto* completionOrderValue = root == nullptr
        ? nullptr
        : field(*root, "completionOrder");
    const auto* format = formatValue == nullptr ? nullptr : formatValue->getIf<std::string>();
    const auto* wireVersion = wireVersionValue == nullptr
        ? nullptr
        : wireVersionValue->getIf<std::int64_t>();
    const auto* workflowIdText = workflowIdValue == nullptr
        ? nullptr
        : workflowIdValue->getIf<std::string>();
    const auto* workflowText = workflowValue == nullptr
        ? nullptr
        : workflowValue->getIf<std::string>();
    const auto* definitionDigestText = definitionDigestValue == nullptr
        ? nullptr
        : definitionDigestValue->getIf<std::string>();
    const auto* stateText = stateValue == nullptr ? nullptr : stateValue->getIf<std::string>();
    const auto* sessionText = sessionValue == nullptr ? nullptr : sessionValue->getIf<std::string>();
    const auto* projectText = projectValue == nullptr ? nullptr : projectValue->getIf<std::string>();
    const auto* documentText = documentValue == nullptr
        ? nullptr
        : documentValue->getIf<std::string>();
    const auto* correlationText = correlationValue == nullptr
        ? nullptr
        : correlationValue->getIf<std::string>();
    const auto* traceText = traceValue == nullptr ? nullptr : traceValue->getIf<std::string>();
    const auto* stepValues = stepsValue == nullptr
        ? nullptr
        : stepsValue->getIf<foundation::Value::Array>();
    const auto* compensationValues = compensationErrorsValue == nullptr
        ? nullptr
        : compensationErrorsValue->getIf<foundation::Value::Array>();
    const auto* completionValues = completionOrderValue == nullptr
        ? nullptr
        : completionOrderValue->getIf<foundation::Value::Array>();
    if(format == nullptr || *format != "lasercnc.workflow-checkpoint"
       || wireVersion == nullptr || *wireVersion != 1 || workflowIdText == nullptr
       || *workflowIdText != indexedId.value().value() || workflowText == nullptr
       || *workflowText != indexedName.value().value() || definitionDigestText == nullptr
       || *definitionDigestText != indexedDefinitionDigest.value().value()
       || stateText == nullptr || *stateText != indexedState.value() || version == nullptr
       || input == nullptr || variables == nullptr
       || variables->kind() != foundation::Value::Kind::Object || sessionText == nullptr
       || projectText == nullptr || documentText == nullptr || correlationText == nullptr
       || traceText == nullptr || parentSpanValue == nullptr || deadlineValue == nullptr
       || stepValues == nullptr || resultValue == nullptr || errorPayload == nullptr
       || compensationValues == nullptr || completionValues == nullptr) {
        return foundation::Result<WorkflowCheckpoint>::failure(workflowPersistenceError(
            "Persistence.WorkflowMetadataMismatch",
            foundation::ErrorCategory::Infrastructure,
            "A workflow payload does not match its control-plane metadata"));
    }
    auto parsedVersion = parseVersion(*version);
    auto state = parseWorkflowState(*stateText);
    auto sessionId = idText<kernel::SessionId>(*sessionText, "sessionId");
    auto projectId = idText<kernel::ProjectId>(*projectText, "projectId");
    auto documentId = idText<kernel::DocumentId>(*documentText, "documentId");
    auto correlationId = idText<kernel::CorrelationId>(*correlationText, "correlationId");
    auto traceId = idText<kernel::TraceId>(*traceText, "traceId");
    auto deadline = parseOptionalTime(*deadlineValue);
    auto error = parseError(*errorPayload);
    const foundation::Version indexedVersion {
        static_cast<std::uint32_t>(major.value()),
        static_cast<std::uint32_t>(minor.value()),
        static_cast<std::uint32_t>(patch.value())};
    if(!parsedVersion || parsedVersion.value() != indexedVersion || !state || !sessionId
       || !projectId || !documentId || !correlationId || !traceId || !deadline || !error) {
        return foundation::Result<WorkflowCheckpoint>::failure(workflowPersistenceError(
            "Persistence.InvalidWorkflowPayload",
            foundation::ErrorCategory::Infrastructure,
            "A workflow payload contains invalid typed fields"));
    }
    std::optional<kernel::SpanId> parentSpanId;
    if(parentSpanValue->kind() != foundation::Value::Kind::Null) {
        const auto* text = parentSpanValue->getIf<std::string>();
        if(text == nullptr) {
            return foundation::Result<WorkflowCheckpoint>::failure(workflowPersistenceError(
                "Persistence.InvalidWorkflowPayload",
                foundation::ErrorCategory::Infrastructure,
                "A workflow parent SpanId is invalid"));
        }
        auto parsed = idText<kernel::SpanId>(*text, "parentSpanId");
        if(!parsed) {
            return foundation::Result<WorkflowCheckpoint>::failure(std::move(parsed).error());
        }
        parentSpanId = std::move(parsed).value();
    }
    std::vector<runtime::WorkflowStepSnapshot> steps;
    steps.reserve(stepValues->size());
    for(const auto& value : *stepValues) {
        auto step = parseStepSnapshot(value);
        if(!step) {
            return foundation::Result<WorkflowCheckpoint>::failure(std::move(step).error());
        }
        steps.push_back(std::move(step).value());
    }
    if(!std::ranges::is_sorted(steps, {}, &runtime::WorkflowStepSnapshot::stepId)
       || std::ranges::adjacent_find(
              steps,
              {},
              &runtime::WorkflowStepSnapshot::stepId)
           != steps.end()) {
        return foundation::Result<WorkflowCheckpoint>::failure(workflowPersistenceError(
            "Persistence.InvalidWorkflowPayload",
            foundation::ErrorCategory::Infrastructure,
            "Workflow step checkpoints must be unique and sorted"));
    }
    std::vector<foundation::Error> compensationErrors;
    compensationErrors.reserve(compensationValues->size());
    for(const auto& value : *compensationValues) {
        auto parsed = parseError(value);
        if(!parsed || !parsed.value().has_value()) {
            return foundation::Result<WorkflowCheckpoint>::failure(workflowPersistenceError(
                "Persistence.InvalidWorkflowPayload",
                foundation::ErrorCategory::Infrastructure,
                "A workflow compensation error is invalid"));
        }
        compensationErrors.push_back(std::move(*parsed.value()));
    }
    std::vector<kernel::WorkflowStepId> completionOrder;
    completionOrder.reserve(completionValues->size());
    for(const auto& value : *completionValues) {
        const auto* text = value.getIf<std::string>();
        if(text == nullptr) {
            return foundation::Result<WorkflowCheckpoint>::failure(workflowPersistenceError(
                "Persistence.InvalidWorkflowPayload",
                foundation::ErrorCategory::Infrastructure,
                "Workflow completion order contains an invalid step id"));
        }
        auto parsed = idText<kernel::WorkflowStepId>(*text, "completionOrder");
        if(!parsed) {
            return foundation::Result<WorkflowCheckpoint>::failure(std::move(parsed).error());
        }
        completionOrder.push_back(std::move(parsed).value());
    }
    std::optional<foundation::Value> result;
    if(resultValue->kind() != foundation::Value::Kind::Null) {
        result = *resultValue;
    }
    runtime::WorkflowRequest request {
        indexedId.value(),
        indexedName.value(),
        *input,
        std::move(sessionId).value(),
        std::move(projectId).value(),
        std::move(documentId).value(),
        std::move(correlationId).value(),
        std::move(traceId).value(),
        std::move(deadline).value(),
        std::move(parentSpanId)};
    runtime::WorkflowSnapshot snapshot {
        indexedId.value(),
        indexedName.value(),
        indexedVersion,
        state.value(),
        *variables,
        std::move(steps),
        std::move(result),
        std::move(error).value(),
        std::move(compensationErrors)};
    return foundation::Result<WorkflowCheckpoint>::success(WorkflowCheckpoint {
        std::move(request),
        indexedDefinitionDigest.value(),
        std::move(snapshot),
        std::move(completionOrder),
        std::chrono::system_clock::time_point {
            std::chrono::milliseconds {updatedAt.value()}}});
}

foundation::Result<void> validateStepRows(
    const WorkflowCheckpoint& checkpoint,
    const std::vector<platform::PersistenceRow>& rows,
    const foundation::IValueSerializer& serializer,
    const platform::IHashService& hashes)
{
    if(rows.size() != checkpoint.snapshot.steps.size()) {
        return foundation::Result<void>::failure(workflowPersistenceError(
            "Persistence.WorkflowStepSetMismatch",
            foundation::ErrorCategory::Infrastructure,
            "Workflow step rows do not match the instance checkpoint"));
    }
    for(std::size_t index = 0U; index < rows.size(); ++index) {
        auto workflowId = idColumn<kernel::WorkflowId>(rows[index], "workflow_id");
        auto stepId = idColumn<kernel::WorkflowStepId>(rows[index], "step_id");
        auto status = textColumn(rows[index], "status");
        auto payload = textColumn(rows[index], "payload");
        auto digest = idColumn<kernel::ContentDigest>(rows[index], "digest");
        auto updatedAt = integerColumn(rows[index], "updated_at_ms");
        if(!workflowId || !stepId || !status || !payload || !digest || !updatedAt
           || updatedAt.value() < 0 || workflowId.value() != checkpoint.snapshot.workflowId
           || stepId.value() != checkpoint.snapshot.steps[index].stepId
           || status.value() != stepStateName(checkpoint.snapshot.steps[index].state)) {
            return foundation::Result<void>::failure(workflowPersistenceError(
                "Persistence.WorkflowStepMetadataMismatch",
                foundation::ErrorCategory::Infrastructure,
                "A workflow step row does not match the instance checkpoint"));
        }
        auto valid = validateDigest(
            payload.value(),
            digest.value(),
            hashes,
            "Persistence.WorkflowStepDigestMismatch");
        if(!valid) {
            return valid;
        }
        auto decoded = serializer.deserialize(payload.value());
        if(!decoded || decoded.value() != stepSnapshotValue(checkpoint.snapshot.steps[index])) {
            return foundation::Result<void>::failure(workflowPersistenceError(
                "Persistence.WorkflowStepPayloadMismatch",
                foundation::ErrorCategory::Infrastructure,
                "A workflow step payload does not match the instance checkpoint"));
        }
    }
    return foundation::Result<void>::success();
}

constexpr std::string_view instanceSelectColumns =
    "workflow_id,workflow_name,workflow_major,workflow_minor,workflow_patch,"
    "definition_digest,status,payload,digest,updated_at_ms";

} // namespace

foundation::Result<kernel::ContentDigest> PersistenceService::workflowDefinitionDigest(
    const runtime::WorkflowDefinition& definition) const
{
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<kernel::ContentDigest>::failure(workflowPersistenceError(
            "Persistence.NotReady",
            foundation::ErrorCategory::Conflict,
            "Persistence must be initialized before hashing a workflow definition"));
    }
    auto payload = serializer_->serialize(definitionValue(definition));
    if(!payload) {
        return foundation::Result<kernel::ContentDigest>::failure(std::move(payload).error());
    }
    return hashes_->digest(bytes(payload.value()));
}

foundation::Result<void> PersistenceService::saveWorkflowCheckpoint(
    const runtime::WorkflowRequest& request,
    const runtime::WorkflowDefinition& definition,
    const runtime::WorkflowSnapshot& snapshot,
    const std::vector<kernel::WorkflowStepId>& completionOrder)
{
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<void>::failure(workflowPersistenceError(
            "Persistence.NotReady",
            foundation::ErrorCategory::Conflict,
            "Persistence must be initialized before saving workflow checkpoints"));
    }
    if(request.workflowId != snapshot.workflowId || request.workflow != snapshot.workflow
       || request.workflow != definition.descriptor.name
       || snapshot.version != definition.descriptor.version
       || request.input.kind() != foundation::Value::Kind::Object
       || snapshot.variables.kind() != foundation::Value::Kind::Object
       || snapshot.steps.size() != definition.steps.size()
       || !std::ranges::is_sorted(snapshot.steps, {}, &runtime::WorkflowStepSnapshot::stepId)
       || std::ranges::adjacent_find(
              snapshot.steps,
              {},
              &runtime::WorkflowStepSnapshot::stepId)
           != snapshot.steps.end()) {
        return foundation::Result<void>::failure(workflowPersistenceError(
            "Persistence.InvalidWorkflowCheckpoint",
            foundation::ErrorCategory::Validation,
            "A workflow checkpoint does not match its request or definition"));
    }
    for(std::size_t index = 0U; index < definition.steps.size(); ++index) {
        if(definition.steps[index].stepId != snapshot.steps[index].stepId) {
            return foundation::Result<void>::failure(workflowPersistenceError(
                "Persistence.InvalidWorkflowCheckpoint",
                foundation::ErrorCategory::Validation,
                "A workflow checkpoint step set does not match its definition"));
        }
    }
    std::vector<kernel::WorkflowStepId> uniqueCompletion = completionOrder;
    std::ranges::sort(uniqueCompletion);
    if(std::ranges::adjacent_find(uniqueCompletion) != uniqueCompletion.end()
       || std::ranges::any_of(completionOrder, [&](const auto& completed) {
              return std::ranges::none_of(
                  snapshot.steps,
                  [&](const auto& step) { return step.stepId == completed; });
          })) {
        return foundation::Result<void>::failure(workflowPersistenceError(
            "Persistence.InvalidWorkflowCheckpoint",
            foundation::ErrorCategory::Validation,
            "Workflow completion order contains duplicate or unknown step identities"));
    }
    bool transactionOpen = false;
    try {
        auto definitionPayload = serializer_->serialize(definitionValue(definition));
        if(!definitionPayload) {
            return foundation::Result<void>::failure(std::move(definitionPayload).error());
        }
        auto definitionDigest = hashes_->digest(bytes(definitionPayload.value()));
        if(!definitionDigest) {
            return foundation::Result<void>::failure(std::move(definitionDigest).error());
        }
        auto payload = serializer_->serialize(checkpointValue(
            request, definitionDigest.value(), snapshot, completionOrder));
        if(!payload) {
            return foundation::Result<void>::failure(std::move(payload).error());
        }
        auto digest = hashes_->digest(bytes(payload.value()));
        if(!digest) {
            return foundation::Result<void>::failure(std::move(digest).error());
        }
        struct EncodedStep final {
            const runtime::WorkflowStepSnapshot* snapshot;
            std::string payload;
            kernel::ContentDigest digest;
        };
        std::vector<EncodedStep> steps;
        steps.reserve(snapshot.steps.size());
        for(const auto& step : snapshot.steps) {
            auto stepPayload = serializer_->serialize(stepSnapshotValue(step));
            if(!stepPayload) {
                return foundation::Result<void>::failure(std::move(stepPayload).error());
            }
            auto stepDigest = hashes_->digest(bytes(stepPayload.value()));
            if(!stepDigest) {
                return foundation::Result<void>::failure(std::move(stepDigest).error());
            }
            steps.push_back(EncodedStep {
                &step, std::move(stepPayload).value(), std::move(stepDigest).value()});
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if(now < 0) {
            return foundation::Result<void>::failure(workflowPersistenceError(
                "Persistence.InvalidTimestamp",
                foundation::ErrorCategory::Internal,
                "The system clock produced an unsupported workflow timestamp"));
        }
        auto begun = backend_->beginTransaction();
        if(!begun) {
            return foundation::Result<void>::failure(std::move(begun).error());
        }
        transactionOpen = true;
        const std::array identityParameters {
            foundation::Value {std::string(request.workflowId.value())}};
        auto existing = backend_->query(
            "SELECT workflow_name,workflow_major,workflow_minor,workflow_patch,definition_digest "
            "FROM workflow_instances WHERE workflow_id=?",
            identityParameters);
        if(!existing) {
            return rollback(*backend_, std::move(existing).error());
        }
        if(existing.value().size() > 1U) {
            return rollback(*backend_, workflowPersistenceError(
                "Persistence.WorkflowIdentityConflict",
                foundation::ErrorCategory::Conflict,
                "A workflow identity resolved to multiple rows"));
        }
        if(!existing.value().empty()) {
            auto name = textColumn(existing.value().front(), "workflow_name");
            auto major = integerColumn(existing.value().front(), "workflow_major");
            auto minor = integerColumn(existing.value().front(), "workflow_minor");
            auto patch = integerColumn(existing.value().front(), "workflow_patch");
            auto priorDigest = textColumn(existing.value().front(), "definition_digest");
            if(!name || !major || !minor || !patch || !priorDigest
               || name.value() != request.workflow.value()
               || major.value() != static_cast<std::int64_t>(definition.descriptor.version.major)
               || minor.value() != static_cast<std::int64_t>(definition.descriptor.version.minor)
               || patch.value() != static_cast<std::int64_t>(definition.descriptor.version.patch)
               || priorDigest.value() != definitionDigest.value().value()) {
                return rollback(*backend_, workflowPersistenceError(
                    "Persistence.WorkflowIdentityConflict",
                    foundation::ErrorCategory::Conflict,
                    "A workflow id is already bound to a different definition"));
            }
        }
        const std::array instanceParameters {
            foundation::Value {std::string(request.workflowId.value())},
            foundation::Value {std::string(request.workflow.value())},
            foundation::Value {static_cast<std::int64_t>(definition.descriptor.version.major)},
            foundation::Value {static_cast<std::int64_t>(definition.descriptor.version.minor)},
            foundation::Value {static_cast<std::int64_t>(definition.descriptor.version.patch)},
            foundation::Value {std::string(definitionDigest.value().value())},
            foundation::Value {workflowStateName(snapshot.state)},
            foundation::Value {payload.value()},
            foundation::Value {std::string(digest.value().value())},
            foundation::Value {static_cast<std::int64_t>(now)}};
        auto upserted = backend_->execute(
            "INSERT INTO workflow_instances("
            "workflow_id,workflow_name,workflow_major,workflow_minor,workflow_patch,"
            "definition_digest,status,payload,digest,updated_at_ms) "
            "VALUES(?,?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(workflow_id) DO UPDATE SET "
            "status=excluded.status,payload=excluded.payload,digest=excluded.digest,"
            "updated_at_ms=excluded.updated_at_ms",
            instanceParameters);
        if(!upserted) {
            return rollback(*backend_, std::move(upserted).error());
        }
        auto deleted = backend_->execute(
            "DELETE FROM workflow_steps WHERE workflow_id=?", identityParameters);
        if(!deleted) {
            return rollback(*backend_, std::move(deleted).error());
        }
        for(const auto& step : steps) {
            const std::array stepParameters {
                foundation::Value {std::string(request.workflowId.value())},
                foundation::Value {std::string(step.snapshot->stepId.value())},
                foundation::Value {stepStateName(step.snapshot->state)},
                foundation::Value {step.payload},
                foundation::Value {std::string(step.digest.value())},
                foundation::Value {static_cast<std::int64_t>(now)}};
            auto inserted = backend_->execute(
                "INSERT INTO workflow_steps("
                "workflow_id,step_id,status,payload,digest,updated_at_ms) "
                "VALUES(?,?,?,?,?,?)",
                stepParameters);
            if(!inserted) {
                return rollback(*backend_, std::move(inserted).error());
            }
        }
        auto committed = backend_->commitTransaction();
        if(!committed) {
            return rollback(*backend_, std::move(committed).error());
        }
        transactionOpen = false;
        return foundation::Result<void>::success();
    } catch(const std::exception& exception) {
        auto error = workflowPersistenceError(
            "Persistence.SaveWorkflowFailed",
            foundation::ErrorCategory::Internal,
            "Saving a workflow checkpoint failed unexpectedly",
            {{"reason", foundation::Value {exception.what()}}});
        return transactionOpen ? rollback(*backend_, std::move(error))
                               : foundation::Result<void>::failure(std::move(error));
    } catch(...) {
        auto error = workflowPersistenceError(
            "Persistence.SaveWorkflowFailed",
            foundation::ErrorCategory::Internal,
            "Saving a workflow checkpoint failed unexpectedly");
        return transactionOpen ? rollback(*backend_, std::move(error))
                               : foundation::Result<void>::failure(std::move(error));
    }
}

foundation::Result<std::optional<WorkflowCheckpoint>> PersistenceService::workflowCheckpoint(
    const kernel::WorkflowId& workflowId) const
{
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<std::optional<WorkflowCheckpoint>>::failure(
            workflowPersistenceError(
                "Persistence.NotReady",
                foundation::ErrorCategory::Conflict,
                "Persistence must be initialized before reading workflow checkpoints"));
    }
    const std::array parameters {
        foundation::Value {std::string(workflowId.value())}};
    auto rows = backend_->query(
        "SELECT " + std::string(instanceSelectColumns)
            + " FROM workflow_instances WHERE workflow_id=?",
        parameters);
    if(!rows) {
        return foundation::Result<std::optional<WorkflowCheckpoint>>::failure(
            std::move(rows).error());
    }
    if(rows.value().empty()) {
        return foundation::Result<std::optional<WorkflowCheckpoint>>::success(std::nullopt);
    }
    if(rows.value().size() != 1U) {
        return foundation::Result<std::optional<WorkflowCheckpoint>>::failure(
            workflowPersistenceError(
                "Persistence.InvalidWorkflowRow",
                foundation::ErrorCategory::Infrastructure,
                "A workflow id resolved to multiple checkpoints"));
    }
    auto checkpoint = decodeCheckpoint(
        rows.value().front(), *serializer_, *hashes_);
    if(!checkpoint) {
        return foundation::Result<std::optional<WorkflowCheckpoint>>::failure(
            std::move(checkpoint).error());
    }
    auto stepRows = backend_->query(
        "SELECT workflow_id,step_id,status,payload,digest,updated_at_ms "
        "FROM workflow_steps WHERE workflow_id=? ORDER BY step_id",
        parameters);
    if(!stepRows) {
        return foundation::Result<std::optional<WorkflowCheckpoint>>::failure(
            std::move(stepRows).error());
    }
    auto valid = validateStepRows(
        checkpoint.value(), stepRows.value(), *serializer_, *hashes_);
    if(!valid) {
        return foundation::Result<std::optional<WorkflowCheckpoint>>::failure(
            std::move(valid).error());
    }
    return foundation::Result<std::optional<WorkflowCheckpoint>>::success(
        std::move(checkpoint).value());
}

foundation::Result<std::vector<WorkflowCheckpoint>> PersistenceService::workflowCheckpoints() const
{
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<std::vector<WorkflowCheckpoint>>::failure(
            workflowPersistenceError(
                "Persistence.NotReady",
                foundation::ErrorCategory::Conflict,
                "Persistence must be initialized before reading workflow checkpoints"));
    }
    auto rows = backend_->query(
        "SELECT " + std::string(instanceSelectColumns)
            + " FROM workflow_instances ORDER BY workflow_id");
    if(!rows) {
        return foundation::Result<std::vector<WorkflowCheckpoint>>::failure(
            std::move(rows).error());
    }
    std::vector<WorkflowCheckpoint> result;
    result.reserve(rows.value().size());
    for(const auto& row : rows.value()) {
        auto checkpoint = decodeCheckpoint(row, *serializer_, *hashes_);
        if(!checkpoint) {
            return foundation::Result<std::vector<WorkflowCheckpoint>>::failure(
                std::move(checkpoint).error());
        }
        const std::array parameters {
            foundation::Value {std::string(checkpoint.value().snapshot.workflowId.value())}};
        auto stepRows = backend_->query(
            "SELECT workflow_id,step_id,status,payload,digest,updated_at_ms "
            "FROM workflow_steps WHERE workflow_id=? ORDER BY step_id",
            parameters);
        if(!stepRows) {
            return foundation::Result<std::vector<WorkflowCheckpoint>>::failure(
                std::move(stepRows).error());
        }
        auto valid = validateStepRows(
            checkpoint.value(), stepRows.value(), *serializer_, *hashes_);
        if(!valid) {
            return foundation::Result<std::vector<WorkflowCheckpoint>>::failure(
                std::move(valid).error());
        }
        result.push_back(std::move(checkpoint).value());
    }
    return foundation::Result<std::vector<WorkflowCheckpoint>>::success(std::move(result));
}

} // namespace lasercnc::persistence
