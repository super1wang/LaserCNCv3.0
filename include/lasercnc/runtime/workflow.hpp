#pragma once

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/foundation/schema.hpp>
#include <lasercnc/foundation/value.hpp>
#include <lasercnc/foundation/version.hpp>
#include <lasercnc/kernel/identifiers.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lasercnc::runtime {

enum class WorkflowStepKind : std::uint8_t {
    Command,
    Query,
    WaitTask,
    Assign,
    Assert,
    Barrier
};

enum class WorkflowPredicateKind : std::uint8_t {
    Exists,
    IsTrue,
    Equals,
    NotEquals,
    ArrayNotEmpty
};

struct WorkflowPredicate final {
    WorkflowPredicateKind kind{WorkflowPredicateKind::IsTrue};
    std::string variablePath;
    foundation::Value expected;
};

struct WorkflowRetryPolicy final {
    std::uint32_t maxAttempts{1U};
    std::chrono::milliseconds backoff{0};
    std::vector<std::string> retryableErrorCodes;
};

struct WorkflowCommandCall final {
    kernel::CommandName command;
    foundation::Version version;
    foundation::Value argumentsTemplate;
};

struct WorkflowQueryCall final {
    kernel::QueryName query;
    foundation::Version version;
    foundation::Value argumentsTemplate;
};

struct WorkflowCompensation final {
    WorkflowCommandCall command;
};

struct WorkflowStep final {
    kernel::WorkflowStepId stepId;
    WorkflowStepKind kind{WorkflowStepKind::Barrier};
    std::vector<kernel::WorkflowStepId> dependencies;
    std::optional<WorkflowPredicate> condition;
    std::optional<WorkflowCommandCall> command;
    std::optional<WorkflowQueryCall> query;
    foundation::Value valueTemplate;
    std::string taskIdVariablePath;
    std::string resultBinding;
    std::optional<std::chrono::milliseconds> timeout;
    WorkflowRetryPolicy retry;
    std::optional<WorkflowCompensation> compensation;
};

struct WorkflowDescriptor final {
    kernel::WorkflowName name;
    foundation::Version version;
    foundation::Schema input;
    foundation::Schema result;
};

struct WorkflowDefinition final {
    WorkflowDescriptor descriptor;
    std::vector<WorkflowStep> steps;
    foundation::Value resultTemplate;
};

enum class WorkflowState : std::uint8_t {
    Pending,
    Running,
    Waiting,
    Succeeded,
    Failed,
    CancelRequested,
    Compensating,
    Cancelled,
    Compensated,
    CompensationFailed
};

enum class WorkflowStepState : std::uint8_t {
    Pending,
    Ready,
    Running,
    Waiting,
    Succeeded,
    Skipped,
    Failed,
    Cancelled,
    Compensated,
    CompensationFailed
};

struct WorkflowRequest final {
    kernel::WorkflowId workflowId;
    kernel::WorkflowName workflow;
    foundation::Value input;
    kernel::SessionId sessionId;
    kernel::ProjectId projectId;
    kernel::DocumentId documentId;
    kernel::CorrelationId correlationId;
    kernel::TraceId traceId;
    std::optional<std::chrono::system_clock::time_point> deadline;
    std::optional<kernel::SpanId> parentSpanId;
};

struct WorkflowStepSnapshot final {
    kernel::WorkflowStepId stepId;
    WorkflowStepState state{WorkflowStepState::Pending};
    std::uint32_t attempt{0U};
    std::optional<std::chrono::system_clock::time_point> nextAttemptAt;
    std::optional<kernel::TaskId> taskId;
    std::optional<foundation::Value> result;
    std::optional<foundation::Error> error;
};

struct WorkflowSnapshot final {
    kernel::WorkflowId workflowId;
    kernel::WorkflowName workflow;
    foundation::Version version;
    WorkflowState state{WorkflowState::Pending};
    foundation::Value variables;
    std::vector<WorkflowStepSnapshot> steps;
    std::optional<foundation::Value> result;
    std::optional<foundation::Error> error;
    std::vector<foundation::Error> compensationErrors;
};

[[nodiscard]] bool isTerminal(WorkflowState state) noexcept;
[[nodiscard]] bool isTerminal(WorkflowStepState state) noexcept;

} // namespace lasercnc::runtime
