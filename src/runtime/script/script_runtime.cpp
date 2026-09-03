#include <lasercnc/runtime/script_runtime.hpp>

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/observability/metrics_service.hpp>
#include <lasercnc/observability/trace_service.hpp>
#include <lasercnc/runtime/command_runtime.hpp>
#include <lasercnc/runtime/execution_services.hpp>
#include <lasercnc/runtime/query_runtime.hpp>
#include <lasercnc/runtime/script_registry.hpp>
#include <lasercnc/runtime/task_runtime.hpp>
#include <lasercnc/runtime/workflow_runtime.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
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

struct ScriptObservationContext final {
    kernel::TraceId traceId;
    std::optional<kernel::SpanId> parentSpanId;
    kernel::ScriptName script;
};

const char* scriptStateLabel(ScriptState state) noexcept
{
    switch(state) {
    case ScriptState::Pending:
        return "pending";
    case ScriptState::Running:
        return "running";
    case ScriptState::Waiting:
        return "waiting";
    case ScriptState::Succeeded:
        return "succeeded";
    case ScriptState::Failed:
        return "failed";
    case ScriptState::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

observability::TraceStatus scriptTraceStatus(ScriptState state) noexcept
{
    if(state == ScriptState::Cancelled) {
        return observability::TraceStatus::Cancelled;
    }
    return state == ScriptState::Failed ? observability::TraceStatus::Failed
                                        : observability::TraceStatus::Succeeded;
}

foundation::Result<kernel::SpanId> scriptSpanId(
    const kernel::ScriptExecutionId& executionId)
{
    static std::atomic_ullong sequence {0U};
    return kernel::SpanId::create(
        "span.script." + std::string(executionId.value()) + "."
        + std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed)));
}

void recordScriptMetrics(
    observability::IMetricsService& metrics,
    const char* outcome,
    std::chrono::steady_clock::duration elapsed) noexcept
{
    try {
        const observability::MetricLabels labels {{"outcome", outcome}};
        auto completed = kernel::MetricName::create("kernel.script.advance.completed");
        if(completed) {
            static_cast<void>(metrics.addCounter(std::move(completed).value(), 1.0, labels));
        }
        auto duration = kernel::MetricName::create("kernel.script.advance.duration_ms");
        if(duration) {
            static_cast<void>(metrics.observeHistogram(
                std::move(duration).value(),
                std::chrono::duration<double, std::milli>(elapsed).count(),
                labels));
        }
    } catch(...) {
    }
}

foundation::Error scriptRuntimeError(
    std::string code,
    foundation::ErrorCategory category,
    std::string message,
    const kernel::ScriptExecutionId* executionId = nullptr,
    const kernel::ScriptNodeId* nodeId = nullptr,
    std::shared_ptr<const foundation::Error> cause = nullptr)
{
    foundation::Value::Object details;
    if(executionId != nullptr) {
        details.emplace(
            "executionId", foundation::Value {std::string(executionId->value())});
    }
    if(nodeId != nullptr) {
        details.emplace("nodeId", foundation::Value {std::string(nodeId->value())});
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

const foundation::Value* lookup(
    const foundation::Value::Object& values,
    std::string_view path)
{
    const auto parts = splitPath(path);
    if(parts.empty()) {
        return nullptr;
    }
    const foundation::Value::Object* object = &values;
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

const foundation::Value* lookupScoped(
    const foundation::Value::Object& variables,
    const foundation::Value::Object& locals,
    std::string_view path)
{
    const auto* local = lookup(locals, path);
    return local == nullptr ? lookup(variables, path) : local;
}

foundation::Result<void> bindValue(
    foundation::Value::Object& values,
    std::string_view path,
    foundation::Value value,
    const kernel::ScriptExecutionId& executionId,
    const kernel::ScriptNodeId& nodeId)
{
    if(path.empty()) {
        return foundation::Result<void>::success();
    }
    const auto parts = splitPath(path);
    if(parts.empty() || parts.size() > 32U) {
        return foundation::Result<void>::failure(scriptRuntimeError(
            "Script.InvalidVariablePath",
            foundation::ErrorCategory::Validation,
            "A script variable path is invalid",
            &executionId,
            &nodeId));
    }
    foundation::Value::Object* object = &values;
    for(std::size_t index = 0U; index + 1U < parts.size(); ++index) {
        auto [found, inserted] = object->try_emplace(
            std::string(parts[index]), foundation::Value {foundation::Value::Object {}});
        static_cast<void>(inserted);
        object = found->second.getIf<foundation::Value::Object>();
        if(object == nullptr) {
            return foundation::Result<void>::failure(scriptRuntimeError(
                "Script.VariablePathConflict",
                foundation::ErrorCategory::Conflict,
                "A script variable path crosses a non-object value",
                &executionId,
                &nodeId));
        }
    }
    (*object)[std::string(parts.back())] = std::move(value);
    return foundation::Result<void>::success();
}

foundation::Result<foundation::Value> resolveTemplate(
    const foundation::Value& source,
    const foundation::Value::Object& variables,
    const foundation::Value::Object& locals,
    const kernel::ScriptExecutionId& executionId,
    const kernel::ScriptNodeId& nodeId)
{
    if(const auto* object = source.getIf<foundation::Value::Object>(); object != nullptr) {
        if(object->size() == 1U) {
            const auto reference = object->find("$ref");
            if(reference != object->end()) {
                const auto* path = reference->second.getIf<std::string>();
                const auto* value = path == nullptr
                    ? nullptr
                    : lookupScoped(variables, locals, *path);
                if(value == nullptr) {
                    return foundation::Result<foundation::Value>::failure(
                        scriptRuntimeError(
                            "Script.VariableNotFound",
                            foundation::ErrorCategory::NotFound,
                            "A script variable reference could not be resolved",
                            &executionId,
                            &nodeId));
                }
                return foundation::Result<foundation::Value>::success(*value);
            }
        }
        foundation::Value::Object resolved;
        for(const auto& [key, value] : *object) {
            auto item = resolveTemplate(
                value, variables, locals, executionId, nodeId);
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
            auto item = resolveTemplate(value, variables, locals, executionId, nodeId);
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
    const foundation::Value::Object& locals,
    const kernel::ScriptExecutionId& executionId,
    const kernel::ScriptNodeId& nodeId)
{
    const auto* value = lookupScoped(variables, locals, predicate.variablePath);
    if(predicate.kind == WorkflowPredicateKind::Exists) {
        return foundation::Result<bool>::success(value != nullptr);
    }
    if(value == nullptr) {
        return foundation::Result<bool>::failure(scriptRuntimeError(
            "Script.PredicateVariableNotFound",
            foundation::ErrorCategory::NotFound,
            "A script predicate variable could not be resolved",
            &executionId,
            &nodeId));
    }
    switch(predicate.kind) {
    case WorkflowPredicateKind::Exists:
        return foundation::Result<bool>::success(true);
    case WorkflowPredicateKind::IsTrue: {
        const auto* boolean = value->getIf<bool>();
        if(boolean == nullptr) {
            return foundation::Result<bool>::failure(scriptRuntimeError(
                "Script.PredicateTypeMismatch",
                foundation::ErrorCategory::Validation,
                "An IsTrue script predicate requires a boolean variable",
                &executionId,
                &nodeId));
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
            return foundation::Result<bool>::failure(scriptRuntimeError(
                "Script.PredicateTypeMismatch",
                foundation::ErrorCategory::Validation,
                "An ArrayNotEmpty script predicate requires an array variable",
                &executionId,
                &nodeId));
        }
        return foundation::Result<bool>::success(!array->empty());
    }
    }
    return foundation::Result<bool>::failure(scriptRuntimeError(
        "Script.PredicateUnsupported",
        foundation::ErrorCategory::Internal,
        "The script predicate kind is unsupported",
        &executionId,
        &nodeId));
}

template <typename Id>
foundation::Result<Id> derivedId(
    std::string prefix,
    const kernel::ScriptExecutionId& executionId,
    std::string_view nodeKey)
{
    prefix += std::string(executionId.value());
    prefix += ":";
    prefix += nodeKey;
    return Id::create(std::move(prefix));
}

} // namespace

class ScriptRuntime::Impl final {
public:
    Impl(
        ScriptRegistry& registry,
        CommandRuntime& commands,
        QueryRuntime& queries,
        WorkflowRuntime& workflows,
        TaskRuntime& tasks,
        ExecutionServices& executionServices,
        observability::ITraceService& traces,
        observability::IMetricsService& metrics,
        std::size_t executionNodeLimit,
        std::size_t includeDepthLimit)
        : registry_(registry),
          commands_(commands),
          queries_(queries),
          workflows_(workflows),
          tasks_(tasks),
          executionServices_(executionServices),
          traces_(traces),
          metrics_(metrics),
          executionNodeLimit_(executionNodeLimit),
          includeDepthLimit_(includeDepthLimit)
    {
    }

    struct Instance final {
        Instance(
            ScriptRequest instanceRequest,
            ScriptDefinition instanceDefinition,
            ScriptSnapshot instanceSnapshot)
            : request(std::move(instanceRequest)),
              definition(std::move(instanceDefinition)),
              snapshot(std::move(instanceSnapshot))
        {
        }

        ScriptRequest request;
        ScriptDefinition definition;
        ScriptSnapshot snapshot;
        std::set<std::string, std::less<>> completedNodes;
        std::map<std::string, bool, std::less<>> selectedBranches;
        std::map<std::string, foundation::Value::Array, std::less<>> loopCollections;
        std::map<std::string, std::size_t, std::less<>> loopIndices;
        mutable std::mutex mutex;
        bool advancing{false};
        bool cancellationRequested{false};
        std::optional<kernel::SpanId> activeAdvanceSpanId;
    };

    enum class NodeOutcome : std::uint8_t { Completed, Waiting, Failed };

    foundation::Result<ScriptSnapshot> startScript(ScriptRequest request)
    {
        if(!accepting_.load(std::memory_order_acquire)) {
            return foundation::Result<ScriptSnapshot>::failure(scriptRuntimeError(
                "Script.RuntimeNotAccepting",
                foundation::ErrorCategory::Conflict,
                "The script runtime is not accepting new executions",
                &request.executionId));
        }
        auto definition = registry_.resolve(request.script);
        if(!definition) {
            return foundation::Result<ScriptSnapshot>::failure(std::move(definition).error());
        }
        if(request.input.kind() != foundation::Value::Kind::Object) {
            return foundation::Result<ScriptSnapshot>::failure(scriptRuntimeError(
                "Script.InputNotObject",
                foundation::ErrorCategory::Validation,
                "Script input must be an object variable table",
                &request.executionId));
        }
        auto services = executionServices_.snapshot();
        if(!services) {
            return foundation::Result<ScriptSnapshot>::failure(std::move(services).error());
        }
        auto valid = services.value().schemaValidator->validate(
            definition.value().descriptor.input, request.input);
        if(!valid) {
            return foundation::Result<ScriptSnapshot>::failure(scriptRuntimeError(
                "Script.InputSchemaFailed",
                foundation::ErrorCategory::Validation,
                "Script input failed schema validation",
                &request.executionId,
                nullptr,
                std::make_shared<const foundation::Error>(std::move(valid).error())));
        }
        ScriptSnapshot snapshot {
            request.executionId,
            definition.value().descriptor.name,
            definition.value().descriptor.version,
            ScriptState::Pending,
            request.input,
            0U,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt};
        auto instance = std::make_shared<Instance>(
            std::move(request), std::move(definition).value(), std::move(snapshot));
        const auto id = instance->snapshot.executionId;
        const auto initial = instance->snapshot;
        std::unique_lock lock(instancesMutex_);
        const auto [unused, inserted] = instances_.emplace(id, std::move(instance));
        static_cast<void>(unused);
        if(!inserted) {
            return foundation::Result<ScriptSnapshot>::failure(scriptRuntimeError(
                "Script.ExecutionAlreadyExists",
                foundation::ErrorCategory::Conflict,
                "A script execution with the same stable id already exists",
                &id));
        }
        return foundation::Result<ScriptSnapshot>::success(initial);
    }

    foundation::Result<ScriptSnapshot> snapshot(
        const kernel::ScriptExecutionId& executionId) const
    {
        auto instance = find(executionId);
        if(!instance) {
            return foundation::Result<ScriptSnapshot>::failure(std::move(instance).error());
        }
        std::lock_guard lock(instance.value()->mutex);
        return foundation::Result<ScriptSnapshot>::success(instance.value()->snapshot);
    }

    foundation::Result<ScriptSnapshot> cancel(
        const kernel::ScriptExecutionId& executionId)
    {
        auto instanceResult = find(executionId);
        if(!instanceResult) {
            return foundation::Result<ScriptSnapshot>::failure(
                std::move(instanceResult).error());
        }
        const auto instance = instanceResult.value();
        std::optional<kernel::TaskId> taskId;
        std::optional<kernel::WorkflowId> workflowId;
        ScriptSnapshot result = [&]() {
            std::lock_guard lock(instance->mutex);
            if(!isTerminal(instance->snapshot.state)) {
                instance->cancellationRequested = true;
                instance->snapshot.state = ScriptState::Cancelled;
                instance->snapshot.error = scriptRuntimeError(
                    "Script.Cancelled",
                    foundation::ErrorCategory::Cancellation,
                    "Script cancellation was requested",
                    &executionId);
            }
            taskId = instance->snapshot.waitingTaskId;
            workflowId = instance->snapshot.waitingWorkflowId;
            return instance->snapshot;
        }();
        if(taskId.has_value()) {
            static_cast<void>(tasks_.cancel(*taskId));
        }
        if(workflowId.has_value()) {
            static_cast<void>(workflows_.cancel(*workflowId));
        }
        return foundation::Result<ScriptSnapshot>::success(std::move(result));
    }

    foundation::Result<ScriptSnapshot> advance(
        const kernel::ScriptExecutionId& executionId,
        std::optional<kernel::SpanId> advanceSpanId = std::nullopt)
    {
        if(!accepting_.load(std::memory_order_acquire)) {
            return foundation::Result<ScriptSnapshot>::failure(scriptRuntimeError(
                "Script.RuntimeNotAccepting",
                foundation::ErrorCategory::Conflict,
                "The script runtime is not accepting execution advancement",
                &executionId));
        }
        auto instanceResult = find(executionId);
        if(!instanceResult) {
            return foundation::Result<ScriptSnapshot>::failure(
                std::move(instanceResult).error());
        }
        const auto instance = instanceResult.value();
        std::unique_lock lock(instance->mutex);
        if(isTerminal(instance->snapshot.state)) {
            return foundation::Result<ScriptSnapshot>::success(instance->snapshot);
        }
        if(instance->advancing) {
            return foundation::Result<ScriptSnapshot>::failure(scriptRuntimeError(
                "Script.AdvanceInProgress",
                foundation::ErrorCategory::Conflict,
                "Only one caller may advance a script execution at a time",
                &executionId));
        }
        instance->advancing = true;
        instance->activeAdvanceSpanId = std::move(advanceSpanId);
        instance->snapshot.state = ScriptState::Running;
        instance->snapshot.waitingTaskId.reset();
        instance->snapshot.waitingWorkflowId.reset();
        activeExecutions_.fetch_add(1U, std::memory_order_acq_rel);
        struct Guard final {
            std::shared_ptr<Instance> instance;
            std::unique_lock<std::mutex>& lock;
            std::atomic_size_t& active;
            ~Guard()
            {
                if(!lock.owns_lock()) {
                    lock.lock();
                }
                instance->advancing = false;
                instance->activeAdvanceSpanId.reset();
                active.fetch_sub(1U, std::memory_order_acq_rel);
            }
        } guard {instance, lock, activeExecutions_};

        const foundation::Value::Object noLocals;
        NodeOutcome outcome = NodeOutcome::Failed;
        try {
            outcome = executeNodes(
                *instance,
                instance->definition.nodes,
                "root",
                noLocals,
                1U,
                lock);
        } catch(const std::exception& exception) {
            instance->snapshot.error = scriptRuntimeError(
                "Script.ExecutionFailed",
                foundation::ErrorCategory::Internal,
                "Script execution raised an exception",
                &executionId,
                nullptr,
                std::make_shared<const foundation::Error>(foundation::makeError(
                    "Script.ExecutionException",
                    foundation::ErrorCategory::Internal,
                    exception.what())));
            outcome = NodeOutcome::Failed;
        } catch(...) {
            instance->snapshot.error = scriptRuntimeError(
                "Script.ExecutionFailed",
                foundation::ErrorCategory::Internal,
                "Script execution raised an unknown exception",
                &executionId);
            outcome = NodeOutcome::Failed;
        }

        if(instance->cancellationRequested) {
            instance->snapshot.state = ScriptState::Cancelled;
        } else if(outcome == NodeOutcome::Waiting) {
            instance->snapshot.state = ScriptState::Waiting;
        } else if(outcome == NodeOutcome::Failed) {
            instance->snapshot.state = ScriptState::Failed;
        } else {
            auto* variables = instance->snapshot.variables.getIf<foundation::Value::Object>();
            const auto synthetic = std::move(
                kernel::ScriptNodeId::create("script.result")).value();
            auto result = resolveTemplate(
                instance->definition.resultTemplate,
                *variables,
                noLocals,
                executionId,
                synthetic);
            if(!result) {
                instance->snapshot.error = std::move(result).error();
                instance->snapshot.state = ScriptState::Failed;
            } else {
                auto services = executionServices_.snapshot();
                auto valid = services
                    ? services.value().schemaValidator->validate(
                          instance->definition.descriptor.result, result.value())
                    : foundation::Result<void>::failure(std::move(services).error());
                if(!valid) {
                    instance->snapshot.error = scriptRuntimeError(
                        "Script.ResultSchemaFailed",
                        foundation::ErrorCategory::Validation,
                        "Script result failed schema validation",
                        &executionId,
                        nullptr,
                        std::make_shared<const foundation::Error>(
                            std::move(valid).error()));
                    instance->snapshot.state = ScriptState::Failed;
                } else {
                    instance->snapshot.result = std::move(result).value();
                    instance->snapshot.state = ScriptState::Succeeded;
                }
            }
        }
        return foundation::Result<ScriptSnapshot>::success(instance->snapshot);
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

    std::optional<ScriptObservationContext> observationContext(
        const kernel::ScriptExecutionId& executionId) const
    {
        auto instance = find(executionId);
        if(!instance) {
            return std::nullopt;
        }
        std::lock_guard lock(instance.value()->mutex);
        return ScriptObservationContext {
            instance.value()->request.traceId,
            instance.value()->request.parentSpanId,
            instance.value()->definition.descriptor.name};
    }

    observability::ITraceService& traces() noexcept
    {
        return traces_;
    }

    observability::IMetricsService& metrics() noexcept
    {
        return metrics_;
    }

private:
    foundation::Result<std::shared_ptr<Instance>> find(
        const kernel::ScriptExecutionId& executionId) const
    {
        std::shared_lock lock(instancesMutex_);
        const auto found = instances_.find(executionId);
        if(found == instances_.end()) {
            return foundation::Result<std::shared_ptr<Instance>>::failure(
                scriptRuntimeError(
                    "Script.ExecutionNotFound",
                    foundation::ErrorCategory::NotFound,
                    "The script execution was not found",
                    &executionId));
        }
        return foundation::Result<std::shared_ptr<Instance>>::success(found->second);
    }

    NodeOutcome fail(Instance& instance, foundation::Error error)
    {
        instance.snapshot.error = std::move(error);
        return NodeOutcome::Failed;
    }

    bool consumeBudget(Instance& instance, const ScriptNode& node)
    {
        if(instance.snapshot.executedNodeCount >= executionNodeLimit_) {
            instance.snapshot.error = scriptRuntimeError(
                "Script.ExecutionNodeLimitExceeded",
                foundation::ErrorCategory::Conflict,
                "Script execution exceeded its total node limit",
                &instance.request.executionId,
                &node.nodeId);
            return false;
        }
        ++instance.snapshot.executedNodeCount;
        return true;
    }

    NodeOutcome executeNodes(
        Instance& instance,
        const std::vector<ScriptNode>& nodes,
        std::string_view scope,
        const foundation::Value::Object& locals,
        std::size_t depth,
        std::unique_lock<std::mutex>& lock)
    {
        if(depth > includeDepthLimit_) {
            return fail(instance, scriptRuntimeError(
                "Script.IncludeDepthExceeded",
                foundation::ErrorCategory::Conflict,
                "Script execution exceeded its include depth limit",
                &instance.request.executionId));
        }
        for(const auto& node : nodes) {
            if(instance.cancellationRequested) {
                return NodeOutcome::Failed;
            }
            const auto key = std::string(scope) + "/" + std::string(node.nodeId.value());
            if(instance.completedNodes.contains(key)) {
                continue;
            }
            if(!consumeBudget(instance, node)) {
                return NodeOutcome::Failed;
            }
            const auto outcome = executeNode(instance, node, key, locals, depth, lock);
            if(outcome != NodeOutcome::Completed) {
                return outcome;
            }
            instance.completedNodes.insert(key);
        }
        return NodeOutcome::Completed;
    }

    NodeOutcome executeNode(
        Instance& instance,
        const ScriptNode& node,
        const std::string& key,
        const foundation::Value::Object& locals,
        std::size_t depth,
        std::unique_lock<std::mutex>& lock)
    {
        auto* variables = instance.snapshot.variables.getIf<foundation::Value::Object>();
        if(variables == nullptr) {
            return fail(instance, scriptRuntimeError(
                "Script.VariableStateCorrupt",
                foundation::ErrorCategory::Internal,
                "Script variables are not an object",
                &instance.request.executionId,
                &node.nodeId));
        }
        if(node.kind == ScriptNodeKind::Assign) {
            auto value = resolveTemplate(
                node.valueTemplate,
                *variables,
                locals,
                instance.request.executionId,
                node.nodeId);
            if(!value) {
                return fail(instance, std::move(value).error());
            }
            auto bound = bindValue(
                *variables,
                node.resultBinding,
                std::move(value).value(),
                instance.request.executionId,
                node.nodeId);
            return bound ? NodeOutcome::Completed : fail(instance, std::move(bound).error());
        }
        if(node.kind == ScriptNodeKind::Assert) {
            auto predicate = evaluatePredicate(
                *node.predicate,
                *variables,
                locals,
                instance.request.executionId,
                node.nodeId);
            if(!predicate) {
                return fail(instance, std::move(predicate).error());
            }
            return predicate.value()
                ? NodeOutcome::Completed
                : fail(instance, scriptRuntimeError(
                      "Script.AssertionFailed",
                      foundation::ErrorCategory::Validation,
                      "A script assertion evaluated to false",
                      &instance.request.executionId,
                      &node.nodeId));
        }
        if(node.kind == ScriptNodeKind::If) {
            auto selected = instance.selectedBranches.find(key);
            if(selected == instance.selectedBranches.end()) {
                auto predicate = evaluatePredicate(
                    *node.predicate,
                    *variables,
                    locals,
                    instance.request.executionId,
                    node.nodeId);
                if(!predicate) {
                    return fail(instance, std::move(predicate).error());
                }
                selected = instance.selectedBranches.emplace(key, predicate.value()).first;
            }
            return executeNodes(
                instance,
                selected->second ? node.thenNodes : node.elseNodes,
                key + (selected->second ? "/then" : "/else"),
                locals,
                depth + 1U,
                lock);
        }
        if(node.kind == ScriptNodeKind::ForEach) {
            auto collection = instance.loopCollections.find(key);
            if(collection == instance.loopCollections.end()) {
                auto resolved = resolveTemplate(
                    node.collectionTemplate,
                    *variables,
                    locals,
                    instance.request.executionId,
                    node.nodeId);
                if(!resolved) {
                    return fail(instance, std::move(resolved).error());
                }
                const auto* array = resolved.value().getIf<foundation::Value::Array>();
                if(array == nullptr) {
                    return fail(instance, scriptRuntimeError(
                        "Script.ForEachValueNotArray",
                        foundation::ErrorCategory::Validation,
                        "A script foreach collection must be an array",
                        &instance.request.executionId,
                        &node.nodeId));
                }
                if(array->size() > node.maxIterations) {
                    return fail(instance, scriptRuntimeError(
                        "Script.ForEachLimitExceeded",
                        foundation::ErrorCategory::Conflict,
                        "A script foreach collection exceeds its iteration limit",
                        &instance.request.executionId,
                        &node.nodeId));
                }
                collection = instance.loopCollections.emplace(key, *array).first;
                instance.loopIndices.emplace(key, 0U);
            }
            auto& index = instance.loopIndices.at(key);
            while(index < collection->second.size()) {
                auto iterationLocals = locals;
                auto itemBound = bindValue(
                    iterationLocals,
                    node.itemVariable,
                    collection->second[index],
                    instance.request.executionId,
                    node.nodeId);
                auto indexBound = bindValue(
                    iterationLocals,
                    node.indexVariable,
                    foundation::Value {static_cast<std::int64_t>(index)},
                    instance.request.executionId,
                    node.nodeId);
                if(!itemBound || !indexBound) {
                    return fail(
                        instance,
                        !itemBound ? std::move(itemBound).error()
                                   : std::move(indexBound).error());
                }
                auto outcome = executeNodes(
                    instance,
                    node.body,
                    key + "/iteration-" + std::to_string(index),
                    iterationLocals,
                    depth + 1U,
                    lock);
                if(outcome != NodeOutcome::Completed) {
                    return outcome;
                }
                ++index;
            }
            return NodeOutcome::Completed;
        }
        if(node.kind == ScriptNodeKind::Include) {
            auto included = registry_.resolve(node.include->script);
            if(!included) {
                return fail(instance, std::move(included).error());
            }
            return executeNodes(
                instance,
                included.value().nodes,
                key + "/include-" + std::string(node.include->script.value()),
                locals,
                depth + 1U,
                lock);
        }
        if(node.kind == ScriptNodeKind::Command) {
            return executeCommand(instance, node, key, locals, lock);
        }
        if(node.kind == ScriptNodeKind::Query) {
            return executeQuery(instance, node, key, locals, lock);
        }
        if(node.kind == ScriptNodeKind::Workflow) {
            return executeWorkflow(instance, node, key, locals, lock);
        }
        if(node.kind == ScriptNodeKind::Wait) {
            return executeWait(instance, node, locals, lock);
        }
        return fail(instance, scriptRuntimeError(
            "Script.NodeUnsupported",
            foundation::ErrorCategory::Internal,
            "The script node kind is unsupported",
            &instance.request.executionId,
            &node.nodeId));
    }

    NodeOutcome executeCommand(
        Instance& instance,
        const ScriptNode& node,
        const std::string& key,
        const foundation::Value::Object& locals,
        std::unique_lock<std::mutex>& lock)
    {
        auto* variables = instance.snapshot.variables.getIf<foundation::Value::Object>();
        auto arguments = resolveTemplate(
            node.command->argumentsTemplate,
            *variables,
            locals,
            instance.request.executionId,
            node.nodeId);
        auto requestId = derivedId<kernel::RequestId>(
            "script-command:", instance.request.executionId, key);
        auto idempotency = derivedId<kernel::IdempotencyKey>(
            "script-command:", instance.request.executionId, key);
        if(!arguments || !requestId || !idempotency) {
            return fail(instance, !arguments
                ? std::move(arguments).error()
                : scriptRuntimeError(
                      "Script.DerivedIdentityInvalid",
                      foundation::ErrorCategory::Internal,
                      "A script command identity could not be derived",
                      &instance.request.executionId,
                      &node.nodeId));
        }
        const CommandRequest request {
            std::move(requestId).value(),
            instance.request.sessionId,
            instance.request.projectId,
            instance.request.documentId,
            node.command->command,
            std::move(arguments).value(),
            std::nullopt,
            instance.request.correlationId,
            instance.request.traceId,
            std::move(idempotency).value(),
            instance.activeAdvanceSpanId.has_value()
                ? instance.activeAdvanceSpanId
                : instance.request.parentSpanId};
        lock.unlock();
        auto response = commands_.execute(request);
        lock.lock();
        if(!response) {
            return fail(instance, std::move(response).error());
        }
        variables = instance.snapshot.variables.getIf<foundation::Value::Object>();
        auto resultBound = bindValue(
            *variables,
            node.command->resultBinding,
            response.value().result,
            instance.request.executionId,
            node.nodeId);
        if(!resultBound) {
            return fail(instance, std::move(resultBound).error());
        }
        if(response.value().taskId.has_value()) {
            auto taskBound = bindValue(
                *variables,
                node.command->taskIdBinding,
                foundation::Value {std::string(response.value().taskId->value())},
                instance.request.executionId,
                node.nodeId);
            if(!taskBound) {
                return fail(instance, std::move(taskBound).error());
            }
        }
        if(!node.command->wait || !response.value().taskId.has_value()) {
            return NodeOutcome::Completed;
        }
        return observeTask(
            instance,
            node,
            *response.value().taskId,
            node.command->taskResultBinding,
            lock);
    }

    NodeOutcome executeQuery(
        Instance& instance,
        const ScriptNode& node,
        const std::string& key,
        const foundation::Value::Object& locals,
        std::unique_lock<std::mutex>& lock)
    {
        auto* variables = instance.snapshot.variables.getIf<foundation::Value::Object>();
        auto arguments = resolveTemplate(
            node.query->argumentsTemplate,
            *variables,
            locals,
            instance.request.executionId,
            node.nodeId);
        auto requestId = derivedId<kernel::RequestId>(
            "script-query:", instance.request.executionId, key);
        if(!arguments || !requestId) {
            return fail(instance, !arguments
                ? std::move(arguments).error()
                : std::move(requestId).error());
        }
        const QueryRequest request {
            std::move(requestId).value(),
            instance.request.sessionId,
            instance.request.projectId,
            instance.request.documentId,
            node.query->query,
            std::move(arguments).value(),
            instance.request.correlationId,
            instance.request.traceId,
            instance.activeAdvanceSpanId.has_value()
                ? instance.activeAdvanceSpanId
                : instance.request.parentSpanId};
        lock.unlock();
        auto response = queries_.execute(request);
        lock.lock();
        if(!response) {
            return fail(instance, std::move(response).error());
        }
        variables = instance.snapshot.variables.getIf<foundation::Value::Object>();
        auto bound = bindValue(
            *variables,
            node.query->resultBinding,
            response.value().result,
            instance.request.executionId,
            node.nodeId);
        return bound ? NodeOutcome::Completed : fail(instance, std::move(bound).error());
    }

    NodeOutcome executeWorkflow(
        Instance& instance,
        const ScriptNode& node,
        const std::string& key,
        const foundation::Value::Object& locals,
        std::unique_lock<std::mutex>& lock)
    {
        auto* variables = instance.snapshot.variables.getIf<foundation::Value::Object>();
        auto input = resolveTemplate(
            node.workflow->inputTemplate,
            *variables,
            locals,
            instance.request.executionId,
            node.nodeId);
        auto workflowId = derivedId<kernel::WorkflowId>(
            "script-workflow:", instance.request.executionId, key);
        if(!input || !workflowId) {
            return fail(instance, !input
                ? std::move(input).error()
                : std::move(workflowId).error());
        }
        const auto stableWorkflowId = workflowId.value();
        const WorkflowRequest request {
            stableWorkflowId,
            node.workflow->workflow,
            std::move(input).value(),
            instance.request.sessionId,
            instance.request.projectId,
            instance.request.documentId,
            instance.request.correlationId,
            instance.request.traceId,
            std::nullopt,
            instance.activeAdvanceSpanId.has_value()
                ? instance.activeAdvanceSpanId
                : instance.request.parentSpanId};
        lock.unlock();
        auto started = workflows_.startWorkflow(request);
        if(!started
           && started.error().code.value() == std::string_view {"Workflow.InstanceAlreadyExists"}) {
            started = workflows_.snapshot(stableWorkflowId);
        }
        lock.lock();
        if(!started) {
            return fail(instance, std::move(started).error());
        }
        variables = instance.snapshot.variables.getIf<foundation::Value::Object>();
        auto idBound = bindValue(
            *variables,
            node.workflow->workflowIdBinding,
            foundation::Value {std::string(stableWorkflowId.value())},
            instance.request.executionId,
            node.nodeId);
        if(!idBound) {
            return fail(instance, std::move(idBound).error());
        }
        if(!node.workflow->wait) {
            return NodeOutcome::Completed;
        }
        lock.unlock();
        auto workflow = workflows_.advance(stableWorkflowId);
        lock.lock();
        return observeWorkflow(
            instance, node, stableWorkflowId, node.workflow->resultBinding, std::move(workflow));
    }

    NodeOutcome executeWait(
        Instance& instance,
        const ScriptNode& node,
        const foundation::Value::Object& locals,
        std::unique_lock<std::mutex>& lock)
    {
        const auto* variables = instance.snapshot.variables.getIf<foundation::Value::Object>();
        const auto* identity = lookupScoped(
            *variables, locals, node.wait->identityVariablePath);
        const auto* text = identity == nullptr ? nullptr : identity->getIf<std::string>();
        if(text == nullptr) {
            return fail(instance, scriptRuntimeError(
                "Script.WaitIdentityInvalid",
                foundation::ErrorCategory::Validation,
                "A script wait node requires a string identity variable",
                &instance.request.executionId,
                &node.nodeId));
        }
        if(node.wait->target == ScriptWaitTarget::Task) {
            auto taskId = kernel::TaskId::create(*text);
            if(!taskId) {
                return fail(instance, std::move(taskId).error());
            }
            return observeTask(
                instance, node, taskId.value(), node.wait->resultBinding, lock);
        }
        auto workflowId = kernel::WorkflowId::create(*text);
        if(!workflowId) {
            return fail(instance, std::move(workflowId).error());
        }
        lock.unlock();
        auto workflow = workflows_.advance(workflowId.value());
        lock.lock();
        return observeWorkflow(
            instance, node, workflowId.value(), node.wait->resultBinding, std::move(workflow));
    }

    NodeOutcome observeTask(
        Instance& instance,
        const ScriptNode& node,
        const kernel::TaskId& taskId,
        std::string_view resultBinding,
        std::unique_lock<std::mutex>& lock)
    {
        lock.unlock();
        auto task = tasks_.snapshot(taskId);
        lock.lock();
        if(!task) {
            return fail(instance, std::move(task).error());
        }
        if(task.value().state == TaskState::Succeeded) {
            auto* variables = instance.snapshot.variables.getIf<foundation::Value::Object>();
            auto bound = bindValue(
                *variables,
                resultBinding,
                task.value().result.value_or(foundation::Value {}),
                instance.request.executionId,
                node.nodeId);
            return bound ? NodeOutcome::Completed : fail(instance, std::move(bound).error());
        }
        if(!isTerminal(task.value().state)) {
            instance.snapshot.waitingTaskId = taskId;
            return NodeOutcome::Waiting;
        }
        return fail(instance, task.value().error.value_or(scriptRuntimeError(
            "Script.TaskFailed",
            task.value().state == TaskState::Cancelled
                ? foundation::ErrorCategory::Cancellation
                : foundation::ErrorCategory::Conflict,
            "A task observed by a script did not succeed",
            &instance.request.executionId,
            &node.nodeId)));
    }

    NodeOutcome observeWorkflow(
        Instance& instance,
        const ScriptNode& node,
        const kernel::WorkflowId& workflowId,
        std::string_view resultBinding,
        foundation::Result<WorkflowSnapshot> workflow)
    {
        if(!workflow) {
            return fail(instance, std::move(workflow).error());
        }
        if(workflow.value().state == WorkflowState::Succeeded) {
            auto* variables = instance.snapshot.variables.getIf<foundation::Value::Object>();
            auto bound = bindValue(
                *variables,
                resultBinding,
                workflow.value().result.value_or(foundation::Value {}),
                instance.request.executionId,
                node.nodeId);
            return bound ? NodeOutcome::Completed : fail(instance, std::move(bound).error());
        }
        if(!isTerminal(workflow.value().state)) {
            instance.snapshot.waitingWorkflowId = workflowId;
            return NodeOutcome::Waiting;
        }
        return fail(instance, workflow.value().error.value_or(scriptRuntimeError(
            "Script.WorkflowFailed",
            foundation::ErrorCategory::Conflict,
            "A workflow observed by a script did not succeed",
            &instance.request.executionId,
            &node.nodeId)));
    }

    ScriptRegistry& registry_;
    CommandRuntime& commands_;
    QueryRuntime& queries_;
    WorkflowRuntime& workflows_;
    TaskRuntime& tasks_;
    ExecutionServices& executionServices_;
    observability::ITraceService& traces_;
    observability::IMetricsService& metrics_;
    const std::size_t executionNodeLimit_;
    const std::size_t includeDepthLimit_;
    mutable std::shared_mutex instancesMutex_;
    std::map<kernel::ScriptExecutionId, std::shared_ptr<Instance>> instances_;
    std::atomic_bool accepting_{false};
    std::atomic_size_t activeExecutions_{0U};
};

ScriptRuntime::ScriptRuntime(
    ScriptRegistry& registry,
    CommandRuntime& commands,
    QueryRuntime& queries,
    WorkflowRuntime& workflows,
    TaskRuntime& tasks,
    ExecutionServices& executionServices,
    observability::ITraceService& traces,
    observability::IMetricsService& metrics,
    std::size_t executionNodeLimit,
    std::size_t includeDepthLimit)
    : impl_(std::make_unique<Impl>(
          registry,
          commands,
          queries,
          workflows,
          tasks,
          executionServices,
          traces,
          metrics,
          executionNodeLimit,
          includeDepthLimit))
{
}

ScriptRuntime::~ScriptRuntime() = default;

foundation::Result<ScriptSnapshot> ScriptRuntime::startScript(ScriptRequest request)
{
    return impl_->startScript(std::move(request));
}

foundation::Result<ScriptSnapshot> ScriptRuntime::advance(
    const kernel::ScriptExecutionId& executionId)
{
    const auto startedAt = std::chrono::steady_clock::now();
    const auto context = impl_->observationContext(executionId);
    std::optional<kernel::SpanId> activeSpanId;
    std::unique_ptr<observability::ITraceSpan> span;
    if(context.has_value()) {
        try {
            auto createdSpanId = scriptSpanId(executionId);
            if(createdSpanId) {
                const auto spanId = createdSpanId.value();
                auto started = impl_->traces().startSpan(observability::TraceSpanStart {
                    context->traceId,
                    spanId,
                    context->parentSpanId,
                    "script.advance",
                    foundation::Value::Object {
                        {"script", foundation::Value {std::string(context->script.value())}},
                    }});
                if(started && started.value() != nullptr) {
                    activeSpanId = spanId;
                    span = std::move(started).value();
                }
            }
        } catch(...) {
        }
    }
    auto result = impl_->advance(executionId, activeSpanId);
    const auto outcome = result ? scriptStateLabel(result.value().state) : "error";
    if(span != nullptr) {
        span->end(
            result ? scriptTraceStatus(result.value().state)
                   : observability::TraceStatus::Failed,
            result ? result.value().error
                   : std::optional<foundation::Error> {result.error()});
    }
    recordScriptMetrics(
        impl_->metrics(), outcome, std::chrono::steady_clock::now() - startedAt);
    return result;
}

foundation::Result<ScriptSnapshot> ScriptRuntime::cancel(
    const kernel::ScriptExecutionId& executionId)
{
    return impl_->cancel(executionId);
}

foundation::Result<ScriptSnapshot> ScriptRuntime::snapshot(
    const kernel::ScriptExecutionId& executionId) const
{
    return impl_->snapshot(executionId);
}

std::size_t ScriptRuntime::activeExecutionCount() const noexcept
{
    return impl_->activeExecutionCount();
}

bool ScriptRuntime::accepting() const noexcept
{
    return impl_->accepting();
}

void ScriptRuntime::start() noexcept
{
    impl_->start();
}

void ScriptRuntime::stop() noexcept
{
    impl_->stop();
}

} // namespace lasercnc::runtime
