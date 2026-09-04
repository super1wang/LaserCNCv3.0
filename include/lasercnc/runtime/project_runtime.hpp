#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/kernel/identifiers.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace lasercnc::kernel { class AppKernel; class ExecutionAdmission; }
namespace lasercnc::persistence {
class PersistenceService;
struct ProjectCatalogRecord;
}

namespace lasercnc::runtime {
class DocumentRuntime;
class ProjectRuntime;

enum class ProjectLifecycleState : std::uint8_t { Closed, Opening, Open, Closing, Failed };

struct ProjectLifecycleSnapshot final {
    kernel::ProjectId projectId;
    ProjectLifecycleState state{ProjectLifecycleState::Closed};
    // Short-lived leases only; close also probes scheduler-owned long-lived work.
    // 中文翻译：仅表示短期租约，关闭还会检查调度器持有的长期任务。
    std::size_t activities{0U};
    std::optional<foundation::Error> error;
};

class ProjectActivityLease final {
public:
    ProjectActivityLease() = default;
private:
    struct Token;
    std::shared_ptr<Token> token_;
    [[nodiscard]] bool matches(const kernel::ProjectId& projectId) const noexcept;
    friend class ProjectRuntime;
    friend class DocumentRuntime;
};

class ProjectRuntime final {
public:
    explicit ProjectRuntime(persistence::PersistenceService& persistence) noexcept;
    [[nodiscard]] foundation::Result<ProjectLifecycleSnapshot> create(kernel::ProjectId projectId);
    [[nodiscard]] foundation::Result<ProjectLifecycleSnapshot> open(const kernel::ProjectId& projectId);
    [[nodiscard]] foundation::Result<ProjectLifecycleSnapshot> close(const kernel::ProjectId& projectId);
    [[nodiscard]] foundation::Result<ProjectLifecycleSnapshot> lifecycle(const kernel::ProjectId& projectId) const;
    [[nodiscard]] std::vector<ProjectLifecycleSnapshot> list() const;
    [[nodiscard]] bool accepting() const noexcept;

private:
    friend class kernel::AppKernel;
    friend class DocumentRuntime;
    friend struct ProjectActivityLease::Token;
    kernel::ExecutionAdmission* admission_{nullptr};
    struct Entry final {
        ProjectLifecycleState state{ProjectLifecycleState::Closed};
        std::size_t activities{0U};
        std::optional<foundation::Error> error;
    };
    [[nodiscard]] foundation::Result<void> configureProject(const kernel::ProjectId& projectId);
    [[nodiscard]] foundation::Result<void> adoptCatalog(
        const std::vector<persistence::ProjectCatalogRecord>& records);
    [[nodiscard]] foundation::Result<ProjectActivityLease> acquireActivity(const kernel::ProjectId& projectId) const;
    void releaseActivity(const kernel::ProjectId& projectId) const noexcept;
    [[nodiscard]] foundation::Result<void> persist(const kernel::ProjectId& projectId, ProjectLifecycleState state);
    [[nodiscard]] foundation::Result<ProjectLifecycleSnapshot> fail(
        const kernel::ProjectId& projectId, foundation::Error error);
    [[nodiscard]] foundation::Result<ProjectLifecycleSnapshot> finishOpen(const kernel::ProjectId& projectId);
    void start() noexcept;
    void stop() noexcept;

    persistence::PersistenceService& persistence_;
    DocumentRuntime* documents_{nullptr};
    std::function<std::size_t(const kernel::ProjectId&)> taskBlocker_;
    mutable std::mutex mutex_;
    mutable std::map<kernel::ProjectId, Entry> entries_;
    std::atomic_bool accepting_{false};
};

[[nodiscard]] const char* projectLifecycleStateName(ProjectLifecycleState state) noexcept;
} // namespace lasercnc::runtime
