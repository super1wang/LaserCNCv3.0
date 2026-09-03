#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/schema.hpp>
#include <lasercnc/foundation/value.hpp>
#include <lasercnc/foundation/version.hpp>
#include <lasercnc/kernel/identifiers.hpp>
#include <lasercnc/state/document.hpp>

#include <chrono>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lasercnc::runtime {

enum class TaskState : std::uint8_t {
    Pending,
    Ready,
    Running,
    Succeeded,
    Failed,
    CancelRequested,
    Cancelled,
    Stale
};

enum class ResourceKind : std::uint8_t {
    CPU,
    DiskIO,
    GPU,
    OCCT,
    ProjectRead,
    ProjectWrite,
    MachineController,
    CollisionBackend
};

enum class ResourceAccess : std::uint8_t { Shared, Exclusive };

struct ResourceClaim final {
    ResourceKind kind{ResourceKind::CPU};
    kernel::ResourceId resource;
    ResourceAccess access{ResourceAccess::Shared};
    std::size_t units{1U};

    friend bool operator==(const ResourceClaim&, const ResourceClaim&) = default;
};

struct TaskDescriptor final {
    kernel::TaskName name;
    foundation::Version version;
    foundation::Schema input;
    foundation::Schema result;
};

struct TaskRequest final {
    kernel::TaskId taskId;
    kernel::TaskName task;
    foundation::Value input;
    kernel::TraceId traceId;
    std::optional<kernel::CorrelationId> correlationId;
    std::optional<kernel::ProjectId> projectId;
    std::optional<kernel::DocumentId> documentId;
    std::optional<state::RevisionSet> expectedRevisions;
    std::optional<state::Revision> expectedProjectRevision;
    std::int32_t priority{0};
    std::vector<kernel::TaskId> dependencies;
    std::vector<ResourceClaim> resources;
    std::optional<std::chrono::steady_clock::time_point> deadline;
};

class CancellationToken final {
public:
    CancellationToken() = default;

    [[nodiscard]] bool cancellationRequested() const noexcept;
    [[nodiscard]] bool deadlineExceeded() const noexcept;

private:
    struct State final {
        std::atomic_bool requested{false};
        std::optional<std::chrono::steady_clock::time_point> deadline;
    };
    explicit CancellationToken(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> state_;

    friend class Scheduler;
};

class ProgressReporter final {
public:
    ProgressReporter() = default;

    [[nodiscard]] foundation::Result<void> report(
        double completed,
        std::string message = {}) const;

private:
    using Callback = std::function<foundation::Result<void>(double, std::string)>;
    explicit ProgressReporter(Callback callback);

    Callback callback_;

    friend class Scheduler;
};

struct ResourceContext final {
    std::vector<ResourceClaim> claims;
};

struct TaskContext final {
    CancellationToken cancellation;
    ProgressReporter progress;
    kernel::TraceId traceId;
    ResourceContext resources;
    std::optional<state::Document> document;
};

class ITaskHandler {
public:
    virtual ~ITaskHandler() = default;

    [[nodiscard]] virtual foundation::Result<foundation::Value> execute(
        const TaskRequest& request,
        const TaskContext& context) = 0;
};

struct TaskSnapshot final {
    kernel::TaskId taskId;
    kernel::TaskName task;
    TaskState state{TaskState::Pending};
    double progress{0.0};
    std::string progressMessage;
    kernel::TraceId traceId;
    std::optional<state::RevisionSet> sourceRevisions;
    std::optional<foundation::Value> result;
    std::optional<foundation::Error> error;
};

[[nodiscard]] bool isTerminal(TaskState state) noexcept;

} // namespace lasercnc::runtime
