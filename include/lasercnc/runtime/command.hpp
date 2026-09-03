#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/schema.hpp>
#include <lasercnc/foundation/value.hpp>
#include <lasercnc/foundation/version.hpp>
#include <lasercnc/kernel/identifiers.hpp>
#include <lasercnc/state/revision.hpp>
#include <lasercnc/runtime/transaction.hpp>
#include <lasercnc/runtime/task.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace lasercnc::runtime {

class ApplicationTransaction;

enum class ExecutionMode : std::uint8_t {
    Synchronous,
    Asynchronous
};

enum class SideEffectLevel : std::uint8_t {
    ReadOnly,
    DocumentWrite,
    FileSystemWrite,
    Publish,
    MachineControl,
    Motion,
    LaserControl
};

struct CommandDescriptor final {
    kernel::CommandName name;
    foundation::Version version;
    foundation::Schema arguments;
    foundation::Schema result;
    ExecutionMode executionMode{ExecutionMode::Synchronous};
    SideEffectLevel sideEffect{SideEffectLevel::DocumentWrite};
    kernel::CapabilityId capability;
    bool undoable{false};
    bool deterministic{false};
    bool idempotent{false};
};

struct CommandRequest final {
    kernel::RequestId requestId;
    kernel::SessionId sessionId;
    kernel::ProjectId projectId;
    kernel::DocumentId documentId;
    kernel::CommandName command;
    foundation::Value arguments;
    std::optional<state::Revision> expectedRevision;
    kernel::CorrelationId correlationId;
    kernel::TraceId traceId;
    std::optional<kernel::IdempotencyKey> idempotencyKey;
    std::optional<kernel::SpanId> parentSpanId;

    friend bool operator==(const CommandRequest&, const CommandRequest&) = default;
};

class ICommandHandler {
public:
    virtual ~ICommandHandler() = default;

    [[nodiscard]] virtual foundation::Result<foundation::Value> execute(
        const CommandRequest& request,
        ApplicationTransaction& transaction) = 0;
};

struct AsyncCommandPlan final {
    TaskRequest task;
    foundation::Value acceptance;
};

class IAsyncCommandHandler {
public:
    virtual ~IAsyncCommandHandler() = default;

    [[nodiscard]] virtual foundation::Result<AsyncCommandPlan> prepare(
        const CommandRequest& request) = 0;
};

struct CommandResponse final {
    foundation::Value result;
    std::optional<TransactionCommit> commit;
    std::optional<kernel::TaskId> taskId;
    std::vector<foundation::Error> postExecutionErrors;
    bool replayed{false};
};

} // namespace lasercnc::runtime
