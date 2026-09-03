#include <lasercnc/runtime/workflow_registry.hpp>

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/runtime/command_registry.hpp>
#include <lasercnc/runtime/query_registry.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace lasercnc::runtime {
namespace {

foundation::Error workflowError(
    std::string code,
    foundation::ErrorCategory category,
    std::string message,
    const kernel::WorkflowName& workflow,
    const kernel::WorkflowStepId* step = nullptr)
{
    foundation::Value::Object details {
        {"workflow", foundation::Value {std::string(workflow.value())}},
    };
    if(step != nullptr) {
        details.emplace("step", foundation::Value {std::string(step->value())});
    }
    return foundation::makeError(
        std::move(code), category, std::move(message), foundation::Value {std::move(details)});
}

foundation::Result<void> validateStepShape(
    const WorkflowDefinition& definition,
    const WorkflowStep& step)
{
    const auto fail = [&](const char* code, const char* message) {
        return foundation::Result<void>::failure(workflowError(
            code,
            foundation::ErrorCategory::Validation,
            message,
            definition.descriptor.name,
            &step.stepId));
    };

    if(step.retry.maxAttempts == 0U || step.retry.backoff.count() < 0) {
        return fail("Workflow.InvalidRetryPolicy", "Workflow retry policy is invalid");
    }
    if(step.timeout.has_value() && step.timeout->count() <= 0) {
        return fail("Workflow.InvalidTimeout", "Workflow step timeout must be positive");
    }
    if(step.retry.maxAttempts > 1U && step.kind != WorkflowStepKind::Command) {
        return fail(
            "Workflow.RetryUnsupported",
            "Only command steps may configure more than one attempt");
    }
    if(step.compensation.has_value() && step.kind != WorkflowStepKind::Command) {
        return fail(
            "Workflow.CompensationUnsupported",
            "Only command steps may declare compensation");
    }

    switch(step.kind) {
    case WorkflowStepKind::Command:
        if(!step.command.has_value() || step.query.has_value()) {
            return fail(
                "Workflow.InvalidCommandStep",
                "A command step requires exactly one command call");
        }
        break;
    case WorkflowStepKind::Query:
        if(!step.query.has_value() || step.command.has_value()) {
            return fail(
                "Workflow.InvalidQueryStep",
                "A query step requires exactly one query call");
        }
        break;
    case WorkflowStepKind::WaitTask:
        if(step.taskIdVariablePath.empty() || step.command.has_value() || step.query.has_value()) {
            return fail(
                "Workflow.InvalidWaitStep",
                "A wait step requires a task id variable and no operation call");
        }
        break;
    case WorkflowStepKind::Assign:
        if(step.resultBinding.empty() || step.command.has_value() || step.query.has_value()) {
            return fail(
                "Workflow.InvalidAssignStep",
                "An assign step requires a result binding and no operation call");
        }
        break;
    case WorkflowStepKind::Assert:
        if(!step.condition.has_value() || step.command.has_value() || step.query.has_value()) {
            return fail(
                "Workflow.InvalidAssertStep",
                "An assert step requires a predicate and no operation call");
        }
        break;
    case WorkflowStepKind::Barrier:
        if(step.command.has_value() || step.query.has_value() || step.condition.has_value()
           || !step.resultBinding.empty()) {
            return fail(
                "Workflow.InvalidBarrierStep",
                "A barrier step may only declare dependencies");
        }
        break;
    }

    return foundation::Result<void>::success();
}

foundation::Result<void> validateGraph(const WorkflowDefinition& definition)
{
    std::map<kernel::WorkflowStepId, const WorkflowStep*> steps;
    for(const auto& step : definition.steps) {
        auto shape = validateStepShape(definition, step);
        if(!shape) {
            return shape;
        }
        const auto [unused, inserted] = steps.emplace(step.stepId, &step);
        static_cast<void>(unused);
        if(!inserted) {
            return foundation::Result<void>::failure(workflowError(
                "Workflow.DuplicateStep",
                foundation::ErrorCategory::Conflict,
                "Workflow step ids must be unique",
                definition.descriptor.name,
                &step.stepId));
        }
    }

    for(const auto& [stepId, step] : steps) {
        for(const auto& dependency : step->dependencies) {
            if(!steps.contains(dependency)) {
                return foundation::Result<void>::failure(workflowError(
                    "Workflow.DependencyNotFound",
                    foundation::ErrorCategory::NotFound,
                    "A workflow dependency is not defined",
                    definition.descriptor.name,
                    &stepId));
            }
            if(dependency == stepId) {
                return foundation::Result<void>::failure(workflowError(
                    "Workflow.DependencyCycle",
                    foundation::ErrorCategory::Validation,
                    "A workflow step cannot depend on itself",
                    definition.descriptor.name,
                    &stepId));
            }
        }
    }

    enum class Visit : std::uint8_t { Visiting, Complete };
    std::map<kernel::WorkflowStepId, Visit> visited;
    std::function<bool(const kernel::WorkflowStepId&)> hasCycle;
    hasCycle = [&](const kernel::WorkflowStepId& stepId) {
        const auto found = visited.find(stepId);
        if(found != visited.end()) {
            return found->second == Visit::Visiting;
        }
        visited.emplace(stepId, Visit::Visiting);
        for(const auto& dependency : steps.at(stepId)->dependencies) {
            if(hasCycle(dependency)) {
                return true;
            }
        }
        visited[stepId] = Visit::Complete;
        return false;
    };
    for(const auto& [stepId, unused] : steps) {
        static_cast<void>(unused);
        if(hasCycle(stepId)) {
            return foundation::Result<void>::failure(workflowError(
                "Workflow.DependencyCycle",
                foundation::ErrorCategory::Validation,
                "Workflow dependencies must form an acyclic graph",
                definition.descriptor.name,
                &stepId));
        }
    }
    return foundation::Result<void>::success();
}

} // namespace

WorkflowRegistry::WorkflowRegistry(
    const CommandRegistry& commands,
    const QueryRegistry& queries) noexcept
    : commands_(commands), queries_(queries)
{
}

foundation::Result<void> WorkflowRegistry::registerDefinition(WorkflowDefinition definition)
{
    auto valid = validateGraph(definition);
    if(!valid) {
        return valid;
    }

    std::unique_lock lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::failure(workflowError(
            "Workflow.RegistryFrozen",
            foundation::ErrorCategory::Conflict,
            "Workflow registration is closed",
            definition.descriptor.name));
    }
    const auto name = definition.descriptor.name;
    const auto [unused, inserted] = definitions_.emplace(name, std::move(definition));
    static_cast<void>(unused);
    if(!inserted) {
        return foundation::Result<void>::failure(workflowError(
            "Workflow.AlreadyRegistered",
            foundation::ErrorCategory::Conflict,
            "A workflow with the same stable name is already registered",
            name));
    }
    return foundation::Result<void>::success();
}

foundation::Result<WorkflowDescriptor> WorkflowRegistry::descriptor(
    const kernel::WorkflowName& name) const
{
    auto definition = resolve(name);
    if(!definition) {
        return foundation::Result<WorkflowDescriptor>::failure(std::move(definition).error());
    }
    return foundation::Result<WorkflowDescriptor>::success(
        std::move(definition).value().descriptor);
}

std::vector<WorkflowDescriptor> WorkflowRegistry::descriptors() const
{
    std::shared_lock lock(mutex_);
    std::vector<WorkflowDescriptor> result;
    result.reserve(definitions_.size());
    for(const auto& [unused, definition] : definitions_) {
        static_cast<void>(unused);
        result.push_back(definition.descriptor);
    }
    return result;
}

std::size_t WorkflowRegistry::size() const
{
    std::shared_lock lock(mutex_);
    return definitions_.size();
}

bool WorkflowRegistry::frozen() const
{
    std::shared_lock lock(mutex_);
    return frozen_;
}

foundation::Result<WorkflowDefinition> WorkflowRegistry::resolve(
    const kernel::WorkflowName& name) const
{
    std::shared_lock lock(mutex_);
    const auto found = definitions_.find(name);
    if(found == definitions_.end()) {
        return foundation::Result<WorkflowDefinition>::failure(workflowError(
            "Workflow.NotFound",
            foundation::ErrorCategory::NotFound,
            "The workflow is not registered",
            name));
    }
    return foundation::Result<WorkflowDefinition>::success(found->second);
}

foundation::Result<void> WorkflowRegistry::validateAndFreeze()
{
    std::unique_lock lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::success();
    }

    for(const auto& [unused, definition] : definitions_) {
        static_cast<void>(unused);
        for(const auto& step : definition.steps) {
            if(step.command.has_value()) {
                auto descriptor = commands_.descriptor(step.command->command);
                if(!descriptor) {
                    return foundation::Result<void>::failure(workflowError(
                        "Workflow.CommandNotFound",
                        foundation::ErrorCategory::NotFound,
                        "A workflow command is not registered",
                        definition.descriptor.name,
                        &step.stepId));
                }
                if(descriptor.value().version != step.command->version) {
                    return foundation::Result<void>::failure(workflowError(
                        "Workflow.CommandVersionMismatch",
                        foundation::ErrorCategory::Conflict,
                        "A workflow command version does not match the registry",
                        definition.descriptor.name,
                        &step.stepId));
                }
                if(!descriptor.value().idempotent) {
                    return foundation::Result<void>::failure(workflowError(
                        "Workflow.CommandNotIdempotent",
                        foundation::ErrorCategory::Validation,
                        "Workflow commands must provide stable idempotency",
                        definition.descriptor.name,
                        &step.stepId));
                }
            }
            if(step.query.has_value()) {
                auto descriptor = queries_.descriptor(step.query->query);
                if(!descriptor) {
                    return foundation::Result<void>::failure(workflowError(
                        "Workflow.QueryNotFound",
                        foundation::ErrorCategory::NotFound,
                        "A workflow query is not registered",
                        definition.descriptor.name,
                        &step.stepId));
                }
                if(descriptor.value().version != step.query->version) {
                    return foundation::Result<void>::failure(workflowError(
                        "Workflow.QueryVersionMismatch",
                        foundation::ErrorCategory::Conflict,
                        "A workflow query version does not match the registry",
                        definition.descriptor.name,
                        &step.stepId));
                }
            }
            if(step.compensation.has_value()) {
                auto descriptor = commands_.descriptor(step.compensation->command.command);
                if(!descriptor || descriptor.value().version != step.compensation->command.version
                   || !descriptor.value().idempotent) {
                    return foundation::Result<void>::failure(workflowError(
                        "Workflow.InvalidCompensationCommand",
                        foundation::ErrorCategory::Validation,
                        "A compensation command must exist at the exact version and be idempotent",
                        definition.descriptor.name,
                        &step.stepId));
                }
            }
        }
    }
    frozen_ = true;
    return foundation::Result<void>::success();
}

} // namespace lasercnc::runtime
