#include <lasercnc/runtime/script_registry.hpp>

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/runtime/command_registry.hpp>
#include <lasercnc/runtime/query_registry.hpp>
#include <lasercnc/runtime/workflow_registry.hpp>

#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace lasercnc::runtime {
namespace {

constexpr std::size_t maximumNodeDepth = 32U;
constexpr std::size_t maximumDefinitionNodes = 10000U;
constexpr std::size_t maximumLoopIterations = 10000U;

foundation::Error scriptError(
    std::string code,
    foundation::ErrorCategory category,
    std::string message,
    const kernel::ScriptName& script,
    const kernel::ScriptNodeId* node = nullptr)
{
    foundation::Value::Object details {
        {"script", foundation::Value {std::string(script.value())}},
    };
    if(node != nullptr) {
        details.emplace("node", foundation::Value {std::string(node->value())});
    }
    return foundation::makeError(
        std::move(code), category, std::move(message), foundation::Value {std::move(details)});
}

foundation::Result<void> validateNodeShape(
    const ScriptDefinition& definition,
    const ScriptNode& node)
{
    const auto fail = [&](const char* code, const char* message) {
        return foundation::Result<void>::failure(scriptError(
            code,
            foundation::ErrorCategory::Validation,
            message,
            definition.descriptor.name,
            &node.nodeId));
    };
    const auto callCount = static_cast<std::size_t>(node.command.has_value())
        + static_cast<std::size_t>(node.query.has_value())
        + static_cast<std::size_t>(node.workflow.has_value())
        + static_cast<std::size_t>(node.wait.has_value())
        + static_cast<std::size_t>(node.include.has_value());
    switch(node.kind) {
    case ScriptNodeKind::Command:
        if(!node.command.has_value() || callCount != 1U) {
            return fail("Script.InvalidCommandNode", "A script command node is invalid");
        }
        break;
    case ScriptNodeKind::Query:
        if(!node.query.has_value() || callCount != 1U) {
            return fail("Script.InvalidQueryNode", "A script query node is invalid");
        }
        break;
    case ScriptNodeKind::Workflow:
        if(!node.workflow.has_value() || callCount != 1U) {
            return fail("Script.InvalidWorkflowNode", "A script workflow node is invalid");
        }
        break;
    case ScriptNodeKind::Wait:
        if(!node.wait.has_value() || callCount != 1U
           || node.wait->identityVariablePath.empty()) {
            return fail("Script.InvalidWaitNode", "A script wait node is invalid");
        }
        break;
    case ScriptNodeKind::Assign:
        if(callCount != 0U || node.resultBinding.empty()) {
            return fail("Script.InvalidAssignNode", "A script assign node is invalid");
        }
        break;
    case ScriptNodeKind::Assert:
        if(callCount != 0U || !node.predicate.has_value()) {
            return fail("Script.InvalidAssertNode", "A script assert node is invalid");
        }
        break;
    case ScriptNodeKind::If:
        if(callCount != 0U || !node.predicate.has_value()) {
            return fail("Script.InvalidIfNode", "A script if node is invalid");
        }
        break;
    case ScriptNodeKind::ForEach:
        if(callCount != 0U || node.itemVariable.empty() || node.indexVariable.empty()
           || node.maxIterations == 0U || node.maxIterations > maximumLoopIterations) {
            return fail("Script.InvalidForEachNode", "A script foreach node is invalid");
        }
        break;
    case ScriptNodeKind::Include:
        if(!node.include.has_value() || callCount != 1U) {
            return fail("Script.InvalidIncludeNode", "A script include node is invalid");
        }
        break;
    }
    return foundation::Result<void>::success();
}

foundation::Result<void> validateNodes(
    const ScriptDefinition& definition,
    const std::vector<ScriptNode>& nodes,
    std::size_t depth,
    std::size_t& count,
    std::set<kernel::ScriptNodeId>& identities)
{
    if(depth > maximumNodeDepth) {
        return foundation::Result<void>::failure(scriptError(
            "Script.NodeDepthExceeded",
            foundation::ErrorCategory::Validation,
            "Script node nesting exceeds the configured limit",
            definition.descriptor.name));
    }
    for(const auto& node : nodes) {
        ++count;
        if(count > maximumDefinitionNodes) {
            return foundation::Result<void>::failure(scriptError(
                "Script.NodeCountExceeded",
                foundation::ErrorCategory::Validation,
                "Script definition node count exceeds the configured limit",
                definition.descriptor.name,
                &node.nodeId));
        }
        if(!identities.insert(node.nodeId).second) {
            return foundation::Result<void>::failure(scriptError(
                "Script.DuplicateNode",
                foundation::ErrorCategory::Conflict,
                "Script node ids must be unique within a definition",
                definition.descriptor.name,
                &node.nodeId));
        }
        auto shape = validateNodeShape(definition, node);
        if(!shape) {
            return shape;
        }
        auto thenValid = validateNodes(
            definition, node.thenNodes, depth + 1U, count, identities);
        if(!thenValid) {
            return thenValid;
        }
        auto elseValid = validateNodes(
            definition, node.elseNodes, depth + 1U, count, identities);
        if(!elseValid) {
            return elseValid;
        }
        auto bodyValid = validateNodes(
            definition, node.body, depth + 1U, count, identities);
        if(!bodyValid) {
            return bodyValid;
        }
    }
    return foundation::Result<void>::success();
}

template <typename Callback>
foundation::Result<void> visitNodes(
    const std::vector<ScriptNode>& nodes,
    Callback&& callback)
{
    for(const auto& node : nodes) {
        auto current = callback(node);
        if(!current) {
            return current;
        }
        auto thenResult = visitNodes(node.thenNodes, callback);
        if(!thenResult) {
            return thenResult;
        }
        auto elseResult = visitNodes(node.elseNodes, callback);
        if(!elseResult) {
            return elseResult;
        }
        auto bodyResult = visitNodes(node.body, callback);
        if(!bodyResult) {
            return bodyResult;
        }
    }
    return foundation::Result<void>::success();
}

} // namespace

ScriptRegistry::ScriptRegistry(
    const CommandRegistry& commands,
    const QueryRegistry& queries,
    const WorkflowRegistry& workflows) noexcept
    : commands_(commands), queries_(queries), workflows_(workflows)
{
}

foundation::Result<void> ScriptRegistry::registerDefinition(ScriptDefinition definition)
{
    std::size_t count = 0U;
    std::set<kernel::ScriptNodeId> identities;
    auto valid = validateNodes(definition, definition.nodes, 1U, count, identities);
    if(!valid) {
        return valid;
    }
    std::unique_lock lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::failure(scriptError(
            "Script.RegistryFrozen",
            foundation::ErrorCategory::Conflict,
            "Script registration is closed",
            definition.descriptor.name));
    }
    const auto name = definition.descriptor.name;
    const auto [unused, inserted] = definitions_.emplace(name, std::move(definition));
    static_cast<void>(unused);
    if(!inserted) {
        return foundation::Result<void>::failure(scriptError(
            "Script.AlreadyRegistered",
            foundation::ErrorCategory::Conflict,
            "A script with the same stable name is already registered",
            name));
    }
    return foundation::Result<void>::success();
}

foundation::Result<ScriptDescriptor> ScriptRegistry::descriptor(
    const kernel::ScriptName& name) const
{
    auto definition = resolve(name);
    if(!definition) {
        return foundation::Result<ScriptDescriptor>::failure(std::move(definition).error());
    }
    return foundation::Result<ScriptDescriptor>::success(
        std::move(definition).value().descriptor);
}

std::vector<ScriptDescriptor> ScriptRegistry::descriptors() const
{
    std::shared_lock lock(mutex_);
    std::vector<ScriptDescriptor> result;
    result.reserve(definitions_.size());
    for(const auto& [unused, definition] : definitions_) {
        static_cast<void>(unused);
        result.push_back(definition.descriptor);
    }
    return result;
}

std::size_t ScriptRegistry::size() const
{
    std::shared_lock lock(mutex_);
    return definitions_.size();
}

bool ScriptRegistry::frozen() const
{
    std::shared_lock lock(mutex_);
    return frozen_;
}

foundation::Result<ScriptDefinition> ScriptRegistry::resolve(
    const kernel::ScriptName& name) const
{
    std::shared_lock lock(mutex_);
    const auto found = definitions_.find(name);
    if(found == definitions_.end()) {
        return foundation::Result<ScriptDefinition>::failure(scriptError(
            "Script.NotFound",
            foundation::ErrorCategory::NotFound,
            "The script is not registered",
            name));
    }
    return foundation::Result<ScriptDefinition>::success(found->second);
}

foundation::Result<void> ScriptRegistry::validateAndFreeze()
{
    std::unique_lock lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::success();
    }
    for(const auto& [unused, definition] : definitions_) {
        static_cast<void>(unused);
        auto operations = visitNodes(definition.nodes, [&](const ScriptNode& node) {
            if(node.command.has_value()) {
                auto descriptor = commands_.descriptor(node.command->command);
                if(!descriptor || descriptor.value().version != node.command->version
                   || !descriptor.value().idempotent) {
                    return foundation::Result<void>::failure(scriptError(
                        "Script.InvalidCommandReference",
                        foundation::ErrorCategory::Validation,
                        "A script command must exist at the exact version and be idempotent",
                        definition.descriptor.name,
                        &node.nodeId));
                }
            }
            if(node.query.has_value()) {
                auto descriptor = queries_.descriptor(node.query->query);
                if(!descriptor || descriptor.value().version != node.query->version) {
                    return foundation::Result<void>::failure(scriptError(
                        "Script.InvalidQueryReference",
                        foundation::ErrorCategory::Validation,
                        "A script query must exist at the exact version",
                        definition.descriptor.name,
                        &node.nodeId));
                }
            }
            if(node.workflow.has_value()) {
                auto descriptor = workflows_.descriptor(node.workflow->workflow);
                if(!descriptor || descriptor.value().version != node.workflow->version) {
                    return foundation::Result<void>::failure(scriptError(
                        "Script.InvalidWorkflowReference",
                        foundation::ErrorCategory::Validation,
                        "A script workflow must exist at the exact version",
                        definition.descriptor.name,
                        &node.nodeId));
                }
            }
            if(node.include.has_value()) {
                const auto included = definitions_.find(node.include->script);
                if(included == definitions_.end()
                   || included->second.descriptor.version != node.include->version) {
                    return foundation::Result<void>::failure(scriptError(
                        "Script.InvalidIncludeReference",
                        foundation::ErrorCategory::Validation,
                        "A script include must exist at the exact version",
                        definition.descriptor.name,
                        &node.nodeId));
                }
            }
            return foundation::Result<void>::success();
        });
        if(!operations) {
            return operations;
        }
    }

    enum class Visit : std::uint8_t { Visiting, Complete };
    std::map<kernel::ScriptName, Visit> visited;
    std::function<foundation::Result<void>(const kernel::ScriptName&)> visit;
    visit = [&](const kernel::ScriptName& name) -> foundation::Result<void> {
        const auto prior = visited.find(name);
        if(prior != visited.end()) {
            if(prior->second == Visit::Visiting) {
                return foundation::Result<void>::failure(scriptError(
                    "Script.IncludeCycle",
                    foundation::ErrorCategory::Validation,
                    "Script includes must form an acyclic graph",
                    name));
            }
            return foundation::Result<void>::success();
        }
        visited.emplace(name, Visit::Visiting);
        auto includes = visitNodes(definitions_.at(name).nodes, [&](const ScriptNode& node) {
            return node.include.has_value()
                ? visit(node.include->script)
                : foundation::Result<void>::success();
        });
        if(!includes) {
            return includes;
        }
        visited[name] = Visit::Complete;
        return foundation::Result<void>::success();
    };
    for(const auto& [name, unused] : definitions_) {
        static_cast<void>(unused);
        auto acyclic = visit(name);
        if(!acyclic) {
            return acyclic;
        }
    }
    frozen_ = true;
    return foundation::Result<void>::success();
}

} // namespace lasercnc::runtime
