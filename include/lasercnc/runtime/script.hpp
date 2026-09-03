#pragma once

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/foundation/schema.hpp>
#include <lasercnc/foundation/value.hpp>
#include <lasercnc/foundation/version.hpp>
#include <lasercnc/kernel/identifiers.hpp>
#include <lasercnc/runtime/workflow.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lasercnc::runtime {

enum class ScriptNodeKind : std::uint8_t {
    Command,
    Query,
    Workflow,
    Wait,
    Assign,
    Assert,
    If,
    ForEach,
    Include
};

enum class ScriptWaitTarget : std::uint8_t { Task, Workflow };

struct ScriptCommandCall final {
    kernel::CommandName command;
    foundation::Version version;
    foundation::Value argumentsTemplate;
    bool wait{false};
    std::string resultBinding;
    std::string taskIdBinding;
    std::string taskResultBinding;
};

struct ScriptQueryCall final {
    kernel::QueryName query;
    foundation::Version version;
    foundation::Value argumentsTemplate;
    std::string resultBinding;
};

struct ScriptWorkflowCall final {
    kernel::WorkflowName workflow;
    foundation::Version version;
    foundation::Value inputTemplate;
    bool wait{false};
    std::string workflowIdBinding;
    std::string resultBinding;
};

struct ScriptWait final {
    ScriptWaitTarget target{ScriptWaitTarget::Task};
    std::string identityVariablePath;
    std::string resultBinding;
};

struct ScriptInclude final {
    kernel::ScriptName script;
    foundation::Version version;
};

struct ScriptNode final {
    kernel::ScriptNodeId nodeId;
    ScriptNodeKind kind{ScriptNodeKind::Assign};
    std::optional<ScriptCommandCall> command;
    std::optional<ScriptQueryCall> query;
    std::optional<ScriptWorkflowCall> workflow;
    std::optional<ScriptWait> wait;
    std::optional<ScriptInclude> include;
    foundation::Value valueTemplate;
    std::string resultBinding;
    std::optional<WorkflowPredicate> predicate;
    std::vector<ScriptNode> thenNodes;
    std::vector<ScriptNode> elseNodes;
    foundation::Value collectionTemplate;
    std::string itemVariable;
    std::string indexVariable;
    std::vector<ScriptNode> body;
    std::size_t maxIterations{1000U};
};

struct ScriptDescriptor final {
    kernel::ScriptName name;
    foundation::Version version;
    foundation::Schema input;
    foundation::Schema result;
};

struct ScriptDefinition final {
    ScriptDescriptor descriptor;
    std::vector<ScriptNode> nodes;
    foundation::Value resultTemplate;
};

enum class ScriptState : std::uint8_t {
    Pending,
    Running,
    Waiting,
    Succeeded,
    Failed,
    Cancelled
};

struct ScriptRequest final {
    kernel::ScriptExecutionId executionId;
    kernel::ScriptName script;
    foundation::Value input;
    kernel::SessionId sessionId;
    kernel::ProjectId projectId;
    kernel::DocumentId documentId;
    kernel::CorrelationId correlationId;
    kernel::TraceId traceId;
    std::optional<kernel::SpanId> parentSpanId;
};

struct ScriptSnapshot final {
    kernel::ScriptExecutionId executionId;
    kernel::ScriptName script;
    foundation::Version version;
    ScriptState state{ScriptState::Pending};
    foundation::Value variables;
    std::size_t executedNodeCount{0U};
    std::optional<kernel::TaskId> waitingTaskId;
    std::optional<kernel::WorkflowId> waitingWorkflowId;
    std::optional<foundation::Value> result;
    std::optional<foundation::Error> error;
};

[[nodiscard]] bool isTerminal(ScriptState state) noexcept;

} // namespace lasercnc::runtime
