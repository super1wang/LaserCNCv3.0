#include <lasercnc/runtime/workflow_runtime.hpp>

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/runtime/command_runtime.hpp>
#include <lasercnc/runtime/execution_services.hpp>
#include <lasercnc/runtime/query_runtime.hpp>
#include <lasercnc/runtime/task_runtime.hpp>
#include <lasercnc/runtime/workflow_registry.hpp>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lasercnc::runtime {
namespace {

foundation::Error runtimeError(
    std::string code,
    foundation::ErrorCategory category,
    std::string message,
    const kernel::WorkflowId* workflowId = nullptr,
    const kernel::WorkflowStepId* stepId = nullptr,
    std::shared_ptr<const foundation::Error> cause = nullptr)
{
    foundation::Value::Object details;
    if(workflowId != nullptr) {
        details.emplace(
            "workflowId", foundation::Value {std::string(workflowId->value())});
    }
    if(stepId != nullptr) {
        details.emplace("stepId", foundation::Value {std::string(stepId->value())});
    }
    return foundation::makeError(
        std::move(code),
        category,
        std::move(message),
        foundation::Value {std::move(details)},
        foundation::Severity::Error,
        std::move(cause));
}

std::vector<std::string_view> splitPath(std::string_view path)
{
    std::vector<std::string_view> parts;
    std::size_t begin = 0U;
    while(begin <= path.size()) {
        const auto end = path.find('.', begin);
        const auto part = path.substr(
            begin, end == std::string_view::npos ? path.size() - begin : end - begin);
        if(part.empty()) {
            return {};
        }
        parts.push_back(part);
        if(end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    return parts;
}

const foundation::Value* lookupVariable(
    const foundation::Value::Object& variables,
    std::string_view path)
{
    const auto parts = splitPath(path);
    if(parts.empty()) {
        return nullptr;
    }
    const foundation::Value::Object* object = &variables;
    const foundation::Value* value = nullptr;
    for(std::size_t index = 0U; index < parts.size(); ++index) {
        const auto found = object->find(parts[index]);
        if(found == object->end()) {
            return nullptr;
        }
        value = &found->second;
        if(index + 1U < parts.size()) {
            object = value->getIf<foundation::Value::Object>();
            if(object == nullptr) {
                return nullptr;
            }
        }
    }
    return value;
}

foundation::Result<void> bindVariable(
    foundation::Value::Object& variables,
    std::string_view path,
    foundation::Value value,
    const kernel::WorkflowId& workflowId,
    const kernel::WorkflowStepId& stepId)
{
    if(path.empty()) {
        return foundation::Result<void>::success();
    }
    const auto parts = splitPath(path);
    if(parts.empty() || parts.size() > 32U) {
        return foundation::Result<void>::failure(runtimeError(
            "Workflow.InvalidVariablePath",
            foundation::ErrorCategory::Validation,
            "A workflow variable path is invalid",
            &workflowId,
            &stepId));
    }

    foundation::Value::Object* object = &variables;
    for(std::size_t index = 0U; index + 1U < parts.size(); ++index) {
        auto [found, inserted] = object->try_emplace(
            std::string(parts[index]), foundation::Value {foundation::Value::Object {}});
        static_cast<void>(inserted);
        object = found->second.getIf<foundation::Value::Object>();
        if(object == nullptr) {
            return foundation::Result<void>::failure(runtimeError(
                "Workflow.VariablePathConflict",
                foundation::ErrorCategory::Conflict,
                "A workflow variable path crosses a non-object value",
                &workflowId,
                &stepId));
        }
    }
    (*object)[std::string(parts.back())] = std::move(value);
    return foundation::Result<void>::success();
}

foundation::Result<foundation::Value> resolveTemplate(
    const foundation::Value& source,
    const foundation::Value::Object& variables,
    const kernel::WorkflowId& workflowId,
    const kernel::WorkflowStepId& stepId)
{
    if(const auto* object = source.getIf<foundation::Value::Object>(); object != nullptr) {
        if(object->size() == 1U) {
            const auto reference = object->find("$ref");
            if(reference != object->end()) {
                const auto* path = reference->second.getIf<std::string>();
                const auto* value = path == nullptr ? nullptr : lookupVariable(variables, *path);
                if(value == nullptr) {
                    return foundation::Result<foundation::Value>::failure(runtimeError(
                        "Workflow.VariableNotFound",
                        foundation::ErrorCategory::NotFound,
                        "A workflow variable reference could not be resolved",
                        &workflowId,
                        &stepId));
                }
                return foundation::Result<foundation::Value>::success(*value);
            }
        }
        foundation::Value::Object resolved;
        for(const auto& [key, value] : *object) {
            auto item = resolveTemplate(value, variables, workflowId, stepId);
            if(!item) {
                return item;
            }
            resolved.emplace(key, std::move(item).value());
        }
        return foundation::Result<foundation::Value>::success(
            foundation::Value {std::move(resolved)});
    }
    if(const auto* array = source.getIf<foundation::Value::Array>(); array != nullptr) {
        foundation::Value::Array resolved;
        resolved.reserve(array->size());
        for(const auto& value : *array) {
            auto item = resolveTemplate(value, variables, workflowId, stepId);
            if(!item) {
                return item;
            }
            resolved.push_back(std::move(item).value());
        }
        return foundation::Result<foundation::Value>::success(
            foundation::Value {std::move(resolved)});
    }
    return foundation::Result<foundation::Value>::success(source);
}

foundation::Result<bool> evaluatePredicate(
    const WorkflowPredicate& predicate,
    const foundation::Value::Object& variables,
    const kernel::WorkflowId& workflowId,
    const kernel::WorkflowStepId& stepId)
{
    const auto* value = lookupVariable(variables, predicate.variablePath);
    if(predicate.kind == WorkflowPredicateKind::Exists) {
        return foundation::Result<bool>::success(value != nullptr);
    }
    if(value == nullptr) {
        return foundation::Result<bool>::failure(runtimeError(
            "Workflow.PredicateVariableNotFound",
            foundation::ErrorCategory::NotFound,
            "A workflow predicate variable could not be resolved",
            &workflowId,
            &stepId));
    }
    switch(predicate.kind) {
    case WorkflowPredicateKind::Exists:
        return foundation::Result<bool>::success(true);
    case WorkflowPredicateKind::IsTrue: {
        const auto* boolean = value->getIf<bool>();
        if(boolean == nullptr) {
            return foundation::Result<bool>::failure(runtimeError(
                "Workflow.PredicateTypeMismatch",
                foundation::ErrorCategory::Validation,
                "An IsTrue workflow predicate requires a boolean variable",
                &workflowId,
                &stepId));
        }
        return foundation::Result<bool>::success(*boolean);
    }
    case WorkflowPredicateKind::Equals:
        return foundation::Result<bool>::success(*value == predicate.expected);
    case WorkflowPredicateKind::NotEquals:
        return foundation::Result<bool>::success(!(*value == predicate.expected));
    case WorkflowPredicateKind::ArrayNotEmpty: {
        const auto* array = value->getIf<foundation::Value::Array>();
        if(array == nullptr) {
            return foundation::Result<bool>::failure(runtimeError(
                "Workflow.PredicateTypeMismatch",
                foundation::ErrorCategory::Validation,
                "An ArrayNotEmpty workflow predicate requires an array variable",
                &workflowId,
                &stepId));
        }
        return foundation::Result<bool>::success(!array->empty());
    }
    }
    return foundation::Result<bool>::failure(runtimeError(
        "Workflow.PredicateUnsupported",
        foundation::ErrorCategory::Internal,
        "The workflow predicate kind is unsupported",
        &workflowId,
        &stepId));
}

template <typename Id>
foundation::Result<Id> derivedId(
    std::string prefix,
    const kernel::WorkflowId& workflowId,
    const kernel::WorkflowStepId& stepId,
    std::uint32_t attempt)
{
    prefix += std::string(workflowId.value());
    prefix += ":";
    prefix += std::string(stepId.value());
    prefix += ":";
    prefix += std::to_string(attempt);
    return Id::create(std::move(prefix));
}

bool retryable(const WorkflowRetryPolicy& policy, const foundation::Error& error)
{
    return std::ranges::any_of(policy.retryableErrorCodes, [&](const std::string& code) {
        return code == error.code.value();
    });
}

struct StepOutcome final {
    WorkflowStepState state{WorkflowStepState::Succeeded};
    std::optional<foundation::Value> result;
    std::optional<kernel::TaskId> taskId;
    std::optional<foundation::Error> error;
};

} // namespace

class WorkflowRuntime::Impl final {
public:
    Impl(
        WorkflowRegistry& registry,
        CommandRuntime& commands,
        QueryRuntime& queries,
        TaskRuntime& tasks,
        ExecutionServices& executionServices)
        : registry_(registry),
          commands_(commands),
          queries_(queries),
          tasks_(tasks),
          executionServices_(executionServices)
    {
    }

    struct Instance final {
        Instance(
            WorkflowRequest instanceRequest,
            WorkflowDefinition instanceDefinition,
            WorkflowSnapshot instanceSnapshot)
            : request(std::move(instanceRequest)),
              definition(std::move(instanceDefinition)),
              snapshot(std::move(instanceSnapshot))
        {
        }

        WorkflowRequest request;
        WorkflowDefinition definition;
        WorkflowSnapshot snapshot;
        std::vector<kernel::WorkflowStepId> completionOrder;
        mutable std::mutex mutex;
        bool advancing{false};
        bool cancellationRequested{false};
    };

    foundation::Result<WorkflowSnapshot> startWorkflow(WorkflowRequest request)
    {
        if(!accepting_.load(std::memory_order_acquire)) {
            return foundation::Result<WorkflowSnapshot>::failure(runtimeError(
                "Workflow.RuntimeNotAccepting",
                foundation::ErrorCategory::Conflict,
                "The workflow runtime is not accepting new instances",
                &request.workflowId));
        }
        auto definition = registry_.resolve(request.workflow);
        if(!definition) {
            return foundation::Result<WorkflowSnapshot>::failure(std::move(definition).error());
        }
        const auto* variables = request.input.getIf<foundation::Value::Object>();
        if(variables == nullptr) {
            return foundation::Result<WorkflowSnapshot>::failure(runtimeError(
                "Workflow.InputNotObject",
                foundation::ErrorCategory::Validation,
                "Workflow input must be an object variable table",
                &request.workflowId));
        }
        auto services = executionServices_.snapshot();
        if(!services) {
            return foundation::Result<WorkflowSnapshot>::failure(std::move(services).error());
        }
        auto valid = services.value().schemaValidator->validate(
            definition.value().descriptor.input, request.input);
        if(!valid) {
            return foundation::Result<WorkflowSnapshot>::failure(runtimeError(
                "Workflow.InputSchemaFailed",
                foundation::ErrorCategory::Validation,
                "Workflow input failed schema validation",
                &request.workflowId,
                nullptr,
                std::make_shared<const foundation::Error>(std::move(valid).error())));
        }

        std::vector<WorkflowStepSnapshot> steps;
        steps.reserve(definition.value().steps.size());
        for(const auto& step : definition.value().steps) {
            steps.push_back(WorkflowStepSnapshot {
                step.stepId,
                WorkflowStepState::Pending,
                0U,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt});
        }
        WorkflowSnapshot snapshot {
            request.workflowId,
            definition.value().descriptor.name,
            definition.value().descriptor.version,
            WorkflowState::Pending,
            request.input,
            std::move(steps),
            std::nullopt,
            std::nullopt,
            {}};
        auto instance = std::make_shared<Instance>(
            std::move(request),
            std::move(definition).value(),
            std::move(snapshot));
        const auto initialSnapshot = instance->snapshot;
        const auto id = instance->snapshot.workflowId;
        {
            std::unique_lock lock(instancesMutex_);
            const auto [unused, inserted] = instances_.emplace(id, instance);
            static_cast<void>(unused);
            if(!inserted) {
                return foundation::Result<WorkflowSnapshot>::failure(runtimeError(
                    "Workflow.InstanceAlreadyExists",
                    foundation::ErrorCategory::Conflict,
                    "A workflow instance with the same stable id already exists",
                    &id));
            }
        }
        return foundation::Result<WorkflowSnapshot>::success(initialSnapshot);
    }

    foundation::Result<WorkflowSnapshot> snapshot(const kernel::WorkflowId& workflowId) const
    {
        auto instance = find(workflowId);
        if(!instance) {
            return foundation::Result<WorkflowSnapshot>::failure(std::move(instance).error());
        }
        std::lock_guard lock(instance.value()->mutex);
        return foundation::Result<WorkflowSnapshot>::success(instance.value()->snapshot);
    }

    foundation::Result<WorkflowSnapshot> cancel(const kernel::WorkflowId& workflowId)
    {
        auto instanceResult = find(workflowId);
        if(!instanceResult) {
            return foundation::Result<WorkflowSnapshot>::failure(
                std::move(instanceResult).error());
        }
        const auto instance = instanceResult.value();
        std::vector<kernel::TaskId> taskIds;
        bool alreadyAdvancing = false;
        {
            std::lock_guard lock(instance->mutex);
            if(isTerminal(instance->snapshot.state)) {
                return foundation::Result<WorkflowSnapshot>::success(instance->snapshot);
            }
            instance->cancellationRequested = true;
            instance->snapshot.state = WorkflowState::CancelRequested;
            alreadyAdvancing = instance->advancing;
            for(const auto& step : instance->snapshot.steps) {
                if(step.taskId.has_value()) {
                    taskIds.push_back(*step.taskId);
                }
            }
        }
        for(const auto& taskId : taskIds) {
            static_cast<void>(tasks_.cancel(taskId));
        }
        if(alreadyAdvancing) {
            return snapshot(workflowId);
        }
        return advance(workflowId);
    }

    foundation::Result<WorkflowSnapshot> advance(const kernel::WorkflowId& workflowId)
    {
        if(!accepting_.load(std::memory_order_acquire)) {
            return foundation::Result<WorkflowSnapshot>::failure(runtimeError(
                "Workflow.RuntimeNotAccepting",
                foundation::ErrorCategory::Conflict,
                "The workflow runtime is not accepting workflow advancement",
                &workflowId));
        }
        auto instanceResult = find(workflowId);
        if(!instanceResult) {
            return foundation::Result<WorkflowSnapshot>::failure(
                std::move(instanceResult).error());
        }
        const auto instance = instanceResult.value();
        {
            std::lock_guard lock(instance->mutex);
            if(isTerminal(instance->snapshot.state)) {
                return foundation::Result<WorkflowSnapshot>::success(instance->snapshot);
            }
            if(instance->advancing) {
                return foundation::Result<WorkflowSnapshot>::failure(runtimeError(
                    "Workflow.AdvanceInProgress",
                    foundation::ErrorCategory::Conflict,
                    "Only one caller may advance a workflow instance at a time",
                    &workflowId));
            }
            instance->advancing = true;
        }
        activeExecutions_.fetch_add(1U, std::memory_order_acq_rel);
        struct Guard final {
            std::shared_ptr<Instance> instance;
            std::atomic_size_t& active;
            ~Guard()
            {
                {
                    std::lock_guard lock(instance->mutex);
                    instance->advancing = false;
                }
                active.fetch_sub(1U, std::memory_order_acq_rel);
            }
        } guard {instance, activeExecutions_};

        std::set<kernel::WorkflowStepId> observedThisAdvance;
        while(true) {
            std::optional<WorkflowStep> operation;
            std::uint32_t attempt = 0U;
            foundation::Value::Object variables;
            bool compensation = false;
            bool stateChangedWithoutExecution = false;
            {
                std::lock_guard lock(instance->mutex);
                if(isTerminal(instance->snapshot.state)) {
                    return foundation::Result<WorkflowSnapshot>::success(
                        instance->snapshot);
                }
                auto* variableObject = instance->snapshot.variables.getIf<foundation::Value::Object>();
                if(variableObject == nullptr) {
                    failLocked(
                        *instance,
                        runtimeError(
                            "Workflow.VariableStateCorrupt",
                            foundation::ErrorCategory::Internal,
                            "Workflow variables are not an object",
                            &workflowId));
                    return foundation::Result<WorkflowSnapshot>::success(instance->snapshot);
                }

                if(instance->cancellationRequested
                   && instance->snapshot.state != WorkflowState::Compensating) {
                    for(auto& step : instance->snapshot.steps) {
                        if(step.state == WorkflowStepState::Pending
                           || step.state == WorkflowStepState::Ready
                           || step.state == WorkflowStepState::Waiting) {
                            step.state = WorkflowStepState::Cancelled;
                        }
                    }
                    instance->snapshot.error = runtimeError(
                        "Workflow.Cancelled",
                        foundation::ErrorCategory::Cancellation,
                        "Workflow cancellation was requested",
                        &workflowId);
                    instance->snapshot.state = hasCompensationLocked(*instance)
                        ? WorkflowState::Compensating
                        : WorkflowState::Cancelled;
                    if(instance->snapshot.state == WorkflowState::Cancelled) {
                        return foundation::Result<WorkflowSnapshot>::success(instance->snapshot);
                    }
                }

                if(instance->snapshot.state == WorkflowState::Compensating) {
                    const auto selected = nextCompensationLocked(*instance);
                    if(!selected.has_value()) {
                        instance->snapshot.state = WorkflowState::Compensated;
                        return foundation::Result<WorkflowSnapshot>::success(instance->snapshot);
                    }
                    operation = *selected;
                    compensation = true;
                    auto& stepSnapshot = stepSnapshotLocked(*instance, operation->stepId);
                    stepSnapshot.state = WorkflowStepState::Running;
                    ++stepSnapshot.compensationAttempt;
                    attempt = stepSnapshot.compensationAttempt;
                    variables = *variableObject;
                } else {
                    const auto now = std::chrono::system_clock::now();
                    if(instance->request.deadline.has_value()
                       && now >= *instance->request.deadline) {
                        failLocked(
                            *instance,
                            runtimeError(
                                "Workflow.DeadlineExceeded",
                                foundation::ErrorCategory::Timeout,
                                "The workflow deadline was exceeded",
                                &workflowId));
                        continue;
                    }

                    for(const auto& step : instance->definition.steps) {
                        auto& stepSnapshot = stepSnapshotLocked(*instance, step.stepId);
                        const bool pending = stepSnapshot.state == WorkflowStepState::Pending;
                        const bool waitingTask = stepSnapshot.state == WorkflowStepState::Waiting
                            && step.kind == WorkflowStepKind::WaitTask;
                        const bool waitingRetry = stepSnapshot.state == WorkflowStepState::Waiting
                            && step.kind == WorkflowStepKind::Command
                            && (!stepSnapshot.nextAttemptAt.has_value()
                                || now >= *stepSnapshot.nextAttemptAt);
                        if((!pending && !waitingTask && !waitingRetry)
                           || observedThisAdvance.contains(step.stepId)
                           || !dependenciesCompleteLocked(*instance, step)) {
                            continue;
                        }

                        if(step.kind != WorkflowStepKind::Assert && step.condition.has_value()) {
                            auto condition = evaluatePredicate(
                                *step.condition, *variableObject, workflowId, step.stepId);
                            if(!condition) {
                                stepSnapshot.state = WorkflowStepState::Failed;
                                stepSnapshot.error = condition.error();
                                failLocked(*instance, condition.error());
                                break;
                            }
                            if(!condition.value()) {
                                stepSnapshot.state = WorkflowStepState::Skipped;
                                observedThisAdvance.insert(step.stepId);
                                stateChangedWithoutExecution = true;
                                continue;
                            }
                        }

                        operation = step;
                        if(!waitingTask) {
                            ++stepSnapshot.attempt;
                        }
                        attempt = stepSnapshot.attempt;
                        stepSnapshot.state = WorkflowStepState::Running;
                        stepSnapshot.nextAttemptAt.reset();
                        if(!stepSnapshot.startedAt.has_value()) {
                            stepSnapshot.startedAt = now;
                        }
                        variables = *variableObject;
                        break;
                    }

                    if(instance->snapshot.state == WorkflowState::Compensating
                       || instance->snapshot.state == WorkflowState::Failed) {
                        continue;
                    }
                    if(!operation.has_value()) {
                        if(stateChangedWithoutExecution) {
                            continue;
                        }
                        if(allStepsCompleteLocked(*instance)) {
                            auto result = resolveTemplate(
                                instance->definition.resultTemplate,
                                *variableObject,
                                workflowId,
                                instance->definition.steps.empty()
                                    ? syntheticStepId()
                                    : instance->definition.steps.back().stepId);
                            if(!result) {
                                failLocked(*instance, std::move(result).error());
                                continue;
                            }
                            auto services = executionServices_.snapshot();
                            if(!services) {
                                failLocked(*instance, std::move(services).error());
                                continue;
                            }
                            auto valid = services.value().schemaValidator->validate(
                                instance->definition.descriptor.result, result.value());
                            if(!valid) {
                                failLocked(
                                    *instance,
                                    runtimeError(
                                        "Workflow.ResultSchemaFailed",
                                        foundation::ErrorCategory::Validation,
                                        "Workflow result failed schema validation",
                                        &workflowId,
                                        nullptr,
                                        std::make_shared<const foundation::Error>(
                                            std::move(valid).error())));
                                continue;
                            }
                            instance->snapshot.result = std::move(result).value();
                            instance->snapshot.state = WorkflowState::Succeeded;
                        } else {
                            instance->snapshot.state = WorkflowState::Waiting;
                        }
                        return foundation::Result<WorkflowSnapshot>::success(instance->snapshot);
                    }
                    instance->snapshot.state = WorkflowState::Running;
                }
            }

            StepOutcome outcome = compensation
                ? executeCompensation(*instance, *operation, variables, attempt)
                : executeStep(*instance, *operation, variables, attempt);
            bool cancelTasks = false;
            {
                std::lock_guard lock(instance->mutex);
                auto& stepSnapshot = stepSnapshotLocked(*instance, operation->stepId);
                observedThisAdvance.insert(operation->stepId);

                if(compensation) {
                    if(outcome.state == WorkflowStepState::Succeeded) {
                        stepSnapshot.state = WorkflowStepState::Compensated;
                        continue;
                    }
                    stepSnapshot.state = WorkflowStepState::CompensationFailed;
                    if(outcome.error.has_value()) {
                        stepSnapshot.error = outcome.error;
                        instance->snapshot.compensationErrors.push_back(*outcome.error);
                    }
                    instance->snapshot.state = WorkflowState::CompensationFailed;
                    return foundation::Result<WorkflowSnapshot>::success(instance->snapshot);
                }

                const auto now = std::chrono::system_clock::now();
                if(outcome.state == WorkflowStepState::Succeeded
                   && timedOutLocked(*instance, *operation, stepSnapshot, now)) {
                    outcome.state = WorkflowStepState::Failed;
                    outcome.error = runtimeError(
                        "Workflow.StepTimeout",
                        foundation::ErrorCategory::Timeout,
                        "A workflow step exceeded its timeout",
                        &workflowId,
                        &operation->stepId);
                }

                if(outcome.state == WorkflowStepState::Waiting) {
                    stepSnapshot.state = WorkflowStepState::Waiting;
                    stepSnapshot.taskId = outcome.taskId;
                    instance->snapshot.state = WorkflowState::Waiting;
                    continue;
                }
                if(outcome.state == WorkflowStepState::Succeeded) {
                    auto* variableObject = instance->snapshot.variables.getIf<foundation::Value::Object>();
                    if(outcome.result.has_value()) {
                        auto bound = bindVariable(
                            *variableObject,
                            operation->resultBinding,
                            *outcome.result,
                            workflowId,
                            operation->stepId);
                        if(!bound) {
                            outcome.state = WorkflowStepState::Failed;
                            outcome.error = std::move(bound).error();
                        }
                    }
                    if(outcome.state == WorkflowStepState::Succeeded
                       && outcome.taskId.has_value()) {
                        stepSnapshot.taskId = outcome.taskId;
                        auto bound = bindVariable(
                            *variableObject,
                            operation->taskIdBinding,
                            foundation::Value {std::string(outcome.taskId->value())},
                            workflowId,
                            operation->stepId);
                        if(!bound) {
                            outcome.state = WorkflowStepState::Failed;
                            outcome.error = std::move(bound).error();
                        }
                    }
                }
                if(outcome.state == WorkflowStepState::Succeeded) {
                    stepSnapshot.state = WorkflowStepState::Succeeded;
                    stepSnapshot.result = outcome.result;
                    stepSnapshot.error.reset();
                    if(operation->compensation.has_value()) {
                        instance->completionOrder.push_back(operation->stepId);
                    }
                    continue;
                }

                const auto error = outcome.error.value_or(runtimeError(
                    "Workflow.StepFailed",
                    foundation::ErrorCategory::Internal,
                    "A workflow step failed without an error",
                    &workflowId,
                    &operation->stepId));
                if(operation->kind == WorkflowStepKind::Command
                   && stepSnapshot.attempt < operation->retry.maxAttempts
                   && retryable(operation->retry, error)) {
                    stepSnapshot.state = WorkflowStepState::Waiting;
                    stepSnapshot.error = error;
                    stepSnapshot.nextAttemptAt = now + operation->retry.backoff;
                    instance->snapshot.state = WorkflowState::Waiting;
                    continue;
                }
                stepSnapshot.state = WorkflowStepState::Failed;
                stepSnapshot.error = error;
                failLocked(*instance, error);
                cancelTasks = true;
            }
            if(cancelTasks) {
                cancelKnownTasks(*instance);
            }
        }
    }

    void start() noexcept
    {
        accepting_.store(true, std::memory_order_release);
    }

    void stop() noexcept
    {
        accepting_.store(false, std::memory_order_release);
    }

    std::size_t activeExecutionCount() const noexcept
    {
        return activeExecutions_.load(std::memory_order_acquire);
    }

    bool accepting() const noexcept
    {
        return accepting_.load(std::memory_order_acquire);
    }

private:
    foundation::Result<std::shared_ptr<Instance>> find(
        const kernel::WorkflowId& workflowId) const
    {
        std::shared_lock lock(instancesMutex_);
        const auto found = instances_.find(workflowId);
        if(found == instances_.end()) {
            return foundation::Result<std::shared_ptr<Instance>>::failure(runtimeError(
                "Workflow.InstanceNotFound",
                foundation::ErrorCategory::NotFound,
                "The workflow instance was not found",
                &workflowId));
        }
        return foundation::Result<std::shared_ptr<Instance>>::success(found->second);
    }

    static WorkflowStepSnapshot& stepSnapshotLocked(
        Instance& instance,
        const kernel::WorkflowStepId& stepId)
    {
        return *std::ranges::find_if(instance.snapshot.steps, [&](const auto& snapshot) {
            return snapshot.stepId == stepId;
        });
    }

    static bool dependenciesCompleteLocked(
        const Instance& instance,
        const WorkflowStep& step)
    {
        return std::ranges::all_of(step.dependencies, [&](const auto& dependency) {
            const auto found = std::ranges::find_if(
                instance.snapshot.steps,
                [&](const auto& snapshot) { return snapshot.stepId == dependency; });
            return found != instance.snapshot.steps.end()
                && (found->state == WorkflowStepState::Succeeded
                    || found->state == WorkflowStepState::Skipped);
        });
    }

    static bool allStepsCompleteLocked(const Instance& instance)
    {
        return std::ranges::all_of(instance.snapshot.steps, [](const auto& step) {
            return step.state == WorkflowStepState::Succeeded
                || step.state == WorkflowStepState::Skipped;
        });
    }

    static bool hasCompensationLocked(const Instance& instance)
    {
        return std::ranges::any_of(instance.completionOrder, [&](const auto& stepId) {
            const auto found = std::ranges::find_if(
                instance.definition.steps,
                [&](const auto& step) { return step.stepId == stepId; });
            return found != instance.definition.steps.end() && found->compensation.has_value();
        });
    }

    static std::optional<WorkflowStep> nextCompensationLocked(const Instance& instance)
    {
        for(auto current = instance.completionOrder.rbegin();
            current != instance.completionOrder.rend();
            ++current) {
            const auto stepSnapshot = std::ranges::find_if(
                instance.snapshot.steps,
                [&](const auto& snapshot) { return snapshot.stepId == *current; });
            if(stepSnapshot == instance.snapshot.steps.end()
               || stepSnapshot->state != WorkflowStepState::Succeeded) {
                continue;
            }
            const auto step = std::ranges::find_if(
                instance.definition.steps,
                [&](const auto& definition) { return definition.stepId == *current; });
            if(step != instance.definition.steps.end() && step->compensation.has_value()) {
                return *step;
            }
        }
        return std::nullopt;
    }

    static bool timedOutLocked(
        const Instance& instance,
        const WorkflowStep& step,
        const WorkflowStepSnapshot& snapshot,
        std::chrono::system_clock::time_point now)
    {
        if(instance.request.deadline.has_value() && now >= *instance.request.deadline) {
            return true;
        }
        return step.timeout.has_value() && snapshot.startedAt.has_value()
            && now - *snapshot.startedAt >= *step.timeout;
    }

    static void failLocked(Instance& instance, foundation::Error error)
    {
        instance.snapshot.error = std::move(error);
        instance.snapshot.state = hasCompensationLocked(instance)
            ? WorkflowState::Compensating
            : WorkflowState::Failed;
    }

    static kernel::WorkflowStepId syntheticStepId()
    {
        return std::move(kernel::WorkflowStepId::create("workflow.result")).value();
    }

    StepOutcome executeStep(
        const Instance& instance,
        const WorkflowStep& step,
        const foundation::Value::Object& variables,
        std::uint32_t attempt)
    {
        if(step.kind == WorkflowStepKind::Command) {
            auto arguments = resolveTemplate(
                step.command->argumentsTemplate,
                variables,
                instance.request.workflowId,
                step.stepId);
            if(!arguments) {
                return StepOutcome {
                    WorkflowStepState::Failed,
                    std::nullopt,
                    std::nullopt,
                    std::move(arguments).error()};
            }
            auto requestId = derivedId<kernel::RequestId>(
                "workflow-command:", instance.request.workflowId, step.stepId, attempt);
            auto idempotency = derivedId<kernel::IdempotencyKey>(
                "workflow-command:", instance.request.workflowId, step.stepId, attempt);
            if(!requestId || !idempotency) {
                return StepOutcome {
                    WorkflowStepState::Failed,
                    std::nullopt,
                    std::nullopt,
                    runtimeError(
                        "Workflow.DerivedIdentityInvalid",
                        foundation::ErrorCategory::Internal,
                        "A workflow command identity could not be derived",
                        &instance.request.workflowId,
                        &step.stepId)};
            }
            auto response = commands_.execute(CommandRequest {
                std::move(requestId).value(),
                instance.request.sessionId,
                instance.request.projectId,
                instance.request.documentId,
                step.command->command,
                std::move(arguments).value(),
                std::nullopt,
                instance.request.correlationId,
                instance.request.traceId,
                std::move(idempotency).value(),
                instance.request.parentSpanId});
            if(!response) {
                return StepOutcome {
                    WorkflowStepState::Failed,
                    std::nullopt,
                    std::nullopt,
                    std::move(response).error()};
            }
            return StepOutcome {
                WorkflowStepState::Succeeded,
                response.value().result,
                response.value().taskId,
                std::nullopt};
        }
        if(step.kind == WorkflowStepKind::Query) {
            auto arguments = resolveTemplate(
                step.query->argumentsTemplate,
                variables,
                instance.request.workflowId,
                step.stepId);
            if(!arguments) {
                return StepOutcome {
                    WorkflowStepState::Failed,
                    std::nullopt,
                    std::nullopt,
                    std::move(arguments).error()};
            }
            auto requestId = derivedId<kernel::RequestId>(
                "workflow-query:", instance.request.workflowId, step.stepId, attempt);
            if(!requestId) {
                return StepOutcome {
                    WorkflowStepState::Failed,
                    std::nullopt,
                    std::nullopt,
                    std::move(requestId).error()};
            }
            auto response = queries_.execute(QueryRequest {
                std::move(requestId).value(),
                instance.request.sessionId,
                instance.request.projectId,
                instance.request.documentId,
                step.query->query,
                std::move(arguments).value(),
                instance.request.correlationId,
                instance.request.traceId,
                instance.request.parentSpanId});
            if(!response) {
                return StepOutcome {
                    WorkflowStepState::Failed,
                    std::nullopt,
                    std::nullopt,
                    std::move(response).error()};
            }
            return StepOutcome {
                WorkflowStepState::Succeeded,
                response.value().result,
                std::nullopt,
                std::nullopt};
        }
        if(step.kind == WorkflowStepKind::WaitTask) {
            const auto* taskValue = lookupVariable(variables, step.taskIdVariablePath);
            const auto* taskText = taskValue == nullptr ? nullptr : taskValue->getIf<std::string>();
            if(taskText == nullptr) {
                return StepOutcome {
                    WorkflowStepState::Failed,
                    std::nullopt,
                    std::nullopt,
                    runtimeError(
                        "Workflow.TaskIdVariableInvalid",
                        foundation::ErrorCategory::Validation,
                        "A workflow wait step requires a string TaskId variable",
                        &instance.request.workflowId,
                        &step.stepId)};
            }
            auto taskId = kernel::TaskId::create(*taskText);
            if(!taskId) {
                return StepOutcome {
                    WorkflowStepState::Failed,
                    std::nullopt,
                    std::nullopt,
                    std::move(taskId).error()};
            }
            auto task = tasks_.snapshot(taskId.value());
            if(!task) {
                return StepOutcome {
                    WorkflowStepState::Failed,
                    std::nullopt,
                    taskId.value(),
                    std::move(task).error()};
            }
            if(task.value().state == TaskState::Succeeded) {
                return StepOutcome {
                    WorkflowStepState::Succeeded,
                    task.value().result.value_or(foundation::Value {}),
                    taskId.value(),
                    std::nullopt};
            }
            if(task.value().state == TaskState::Failed) {
                return StepOutcome {
                    WorkflowStepState::Failed,
                    std::nullopt,
                    taskId.value(),
                    task.value().error.value_or(runtimeError(
                        "Workflow.TaskFailed",
                        foundation::ErrorCategory::Internal,
                        "A workflow task failed without an error",
                        &instance.request.workflowId,
                        &step.stepId))};
            }
            if(task.value().state == TaskState::Cancelled) {
                return StepOutcome {
                    WorkflowStepState::Failed,
                    std::nullopt,
                    taskId.value(),
                    runtimeError(
                        "Workflow.TaskCancelled",
                        foundation::ErrorCategory::Cancellation,
                        "A workflow task was cancelled",
                        &instance.request.workflowId,
                        &step.stepId)};
            }
            if(task.value().state == TaskState::Stale) {
                return StepOutcome {
                    WorkflowStepState::Failed,
                    std::nullopt,
                    taskId.value(),
                    runtimeError(
                        "Workflow.TaskStale",
                        foundation::ErrorCategory::Conflict,
                        "A workflow task result became stale",
                        &instance.request.workflowId,
                        &step.stepId)};
            }
            return StepOutcome {
                WorkflowStepState::Waiting,
                std::nullopt,
                taskId.value(),
                std::nullopt};
        }
        if(step.kind == WorkflowStepKind::Assign) {
            auto value = resolveTemplate(
                step.valueTemplate,
                variables,
                instance.request.workflowId,
                step.stepId);
            if(!value) {
                return StepOutcome {
                    WorkflowStepState::Failed,
                    std::nullopt,
                    std::nullopt,
                    std::move(value).error()};
            }
            return StepOutcome {
                WorkflowStepState::Succeeded,
                std::move(value).value(),
                std::nullopt,
                std::nullopt};
        }
        if(step.kind == WorkflowStepKind::Assert) {
            auto predicate = evaluatePredicate(
                *step.condition,
                variables,
                instance.request.workflowId,
                step.stepId);
            if(!predicate) {
                return StepOutcome {
                    WorkflowStepState::Failed,
                    std::nullopt,
                    std::nullopt,
                    std::move(predicate).error()};
            }
            if(!predicate.value()) {
                return StepOutcome {
                    WorkflowStepState::Failed,
                    std::nullopt,
                    std::nullopt,
                    runtimeError(
                        "Workflow.AssertionFailed",
                        foundation::ErrorCategory::Validation,
                        "A workflow assertion evaluated to false",
                        &instance.request.workflowId,
                        &step.stepId)};
            }
        }
        return StepOutcome {
            WorkflowStepState::Succeeded,
            std::nullopt,
            std::nullopt,
            std::nullopt};
    }

    StepOutcome executeCompensation(
        const Instance& instance,
        const WorkflowStep& step,
        const foundation::Value::Object& variables,
        std::uint32_t attempt)
    {
        const auto& call = step.compensation->command;
        auto arguments = resolveTemplate(
            call.argumentsTemplate,
            variables,
            instance.request.workflowId,
            step.stepId);
        if(!arguments) {
            return StepOutcome {
                WorkflowStepState::CompensationFailed,
                std::nullopt,
                std::nullopt,
                std::move(arguments).error()};
        }
        auto requestId = derivedId<kernel::RequestId>(
            "workflow-compensation:", instance.request.workflowId, step.stepId, attempt);
        auto idempotency = derivedId<kernel::IdempotencyKey>(
            "workflow-compensation:", instance.request.workflowId, step.stepId, attempt);
        if(!requestId || !idempotency) {
            return StepOutcome {
                WorkflowStepState::CompensationFailed,
                std::nullopt,
                std::nullopt,
                runtimeError(
                    "Workflow.DerivedIdentityInvalid",
                    foundation::ErrorCategory::Internal,
                    "A workflow compensation identity could not be derived",
                    &instance.request.workflowId,
                    &step.stepId)};
        }
        auto response = commands_.execute(CommandRequest {
            std::move(requestId).value(),
            instance.request.sessionId,
            instance.request.projectId,
            instance.request.documentId,
            call.command,
            std::move(arguments).value(),
            std::nullopt,
            instance.request.correlationId,
            instance.request.traceId,
            std::move(idempotency).value(),
            instance.request.parentSpanId});
        if(!response) {
            return StepOutcome {
                WorkflowStepState::CompensationFailed,
                std::nullopt,
                std::nullopt,
                std::move(response).error()};
        }
        return StepOutcome {
            WorkflowStepState::Succeeded,
            response.value().result,
            response.value().taskId,
            std::nullopt};
    }

    void cancelKnownTasks(Instance& instance)
    {
        std::vector<kernel::TaskId> taskIds;
        {
            std::lock_guard lock(instance.mutex);
            for(const auto& step : instance.snapshot.steps) {
                if(step.taskId.has_value()) {
                    taskIds.push_back(*step.taskId);
                }
            }
        }
        for(const auto& taskId : taskIds) {
            static_cast<void>(tasks_.cancel(taskId));
        }
    }

    WorkflowRegistry& registry_;
    CommandRuntime& commands_;
    QueryRuntime& queries_;
    TaskRuntime& tasks_;
    ExecutionServices& executionServices_;
    mutable std::shared_mutex instancesMutex_;
    std::map<kernel::WorkflowId, std::shared_ptr<Instance>> instances_;
    std::atomic_bool accepting_{false};
    std::atomic_size_t activeExecutions_{0U};
};

WorkflowRuntime::WorkflowRuntime(
    WorkflowRegistry& registry,
    CommandRuntime& commands,
    QueryRuntime& queries,
    TaskRuntime& tasks,
    ExecutionServices& executionServices)
    : impl_(std::make_unique<Impl>(registry, commands, queries, tasks, executionServices))
{
}

WorkflowRuntime::~WorkflowRuntime() = default;

foundation::Result<WorkflowSnapshot> WorkflowRuntime::startWorkflow(WorkflowRequest request)
{
    return impl_->startWorkflow(std::move(request));
}

foundation::Result<WorkflowSnapshot> WorkflowRuntime::advance(
    const kernel::WorkflowId& workflowId)
{
    return impl_->advance(workflowId);
}

foundation::Result<WorkflowSnapshot> WorkflowRuntime::cancel(
    const kernel::WorkflowId& workflowId)
{
    return impl_->cancel(workflowId);
}

foundation::Result<WorkflowSnapshot> WorkflowRuntime::snapshot(
    const kernel::WorkflowId& workflowId) const
{
    return impl_->snapshot(workflowId);
}

std::size_t WorkflowRuntime::activeExecutionCount() const noexcept
{
    return impl_->activeExecutionCount();
}

bool WorkflowRuntime::accepting() const noexcept
{
    return impl_->accepting();
}

void WorkflowRuntime::start() noexcept
{
    impl_->start();
}

void WorkflowRuntime::stop() noexcept
{
    impl_->stop();
}

} // namespace lasercnc::runtime
