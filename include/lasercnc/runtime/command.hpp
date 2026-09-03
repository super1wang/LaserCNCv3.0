#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/schema.hpp>
#include <lasercnc/foundation/value.hpp>
#include <lasercnc/foundation/version.hpp>
#include <lasercnc/kernel/identifiers.hpp>
#include <lasercnc/state/revision.hpp>

#include <cstdint>
#include <optional>

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

    friend bool operator==(const CommandRequest&, const CommandRequest&) = default;
};

class ICommandHandler {
public:
    virtual ~ICommandHandler() = default;

    [[nodiscard]] virtual foundation::Result<foundation::Value> execute(
        const CommandRequest& request,
        ApplicationTransaction& transaction) = 0;
};

} // namespace lasercnc::runtime
