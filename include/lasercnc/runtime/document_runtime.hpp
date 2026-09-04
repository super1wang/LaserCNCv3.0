#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/kernel/identifiers.hpp>
#include <lasercnc/platform/asset_store.hpp>
#include <lasercnc/state/document.hpp>
#include <lasercnc/runtime/project_runtime.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace lasercnc::kernel {
class AppKernel;
class ExecutionAdmission;
}

namespace lasercnc::persistence {
struct DocumentCatalogRecord;
class PersistenceService;
}

namespace lasercnc::state {
class DocumentStore;
class ObjectTypeRegistry;
}

namespace lasercnc::runtime {

class CommandRuntime;
class QueryRuntime;
class ScriptRuntime;
class TaskRuntime;
class TransactionManager;
class WorkflowRuntime;

enum class DocumentLifecycleState : std::uint8_t {
    Detached,
    Opening,
    Open,
    Closing,
    Failed
};

enum class DocumentActivityKind : std::uint8_t {
    Command,
    Query,
    Transaction,
    TaskAdmission,
    WorkflowAdmission,
    ScriptAdmission
};

struct DocumentLifecycleSnapshot final {
    kernel::ProjectId projectId;
    kernel::DocumentId documentId;
    DocumentLifecycleState state{DocumentLifecycleState::Detached};
    std::array<std::size_t, 6U> activities{};
    std::optional<foundation::Error> error;
};

class DocumentActivityLease final {
public:
    DocumentActivityLease() = default;

private:
    explicit DocumentActivityLease(std::shared_ptr<void> token)
        : token_(std::move(token))
    {
    }

    std::shared_ptr<void> token_;

    friend class DocumentRuntime;
};

class DocumentRuntime final {
public:
    DocumentRuntime(
        state::DocumentStore& documents,
        persistence::PersistenceService& persistence,
        const state::ObjectTypeRegistry* objectTypes = nullptr,
        const platform::IAssetStore* assetStore = nullptr) noexcept;

    [[nodiscard]] foundation::Result<DocumentLifecycleSnapshot> create(
        kernel::ProjectId projectId,
        kernel::DocumentId documentId);
    [[nodiscard]] foundation::Result<DocumentLifecycleSnapshot> attach(
        state::DocumentImage image);
    [[nodiscard]] foundation::Result<DocumentLifecycleSnapshot> open(
        const kernel::DocumentId& documentId);
    [[nodiscard]] foundation::Result<state::Document> snapshot(
        const kernel::DocumentId& documentId) const;
    [[nodiscard]] foundation::Result<DocumentLifecycleSnapshot> close(
        const kernel::DocumentId& documentId);
    [[nodiscard]] foundation::Result<DocumentLifecycleSnapshot> detach(
        const kernel::DocumentId& documentId);
    [[nodiscard]] foundation::Result<void> remove(
        const kernel::DocumentId& documentId);
    [[nodiscard]] foundation::Result<DocumentLifecycleSnapshot> lifecycle(
        const kernel::DocumentId& documentId) const;
    [[nodiscard]] std::vector<DocumentLifecycleSnapshot> list() const;
    [[nodiscard]] bool accepting() const noexcept;

private:
    friend class CommandRuntime;
    friend class QueryRuntime;
    friend class ScriptRuntime;
    friend class TaskRuntime;
    friend class TransactionManager;
    friend class WorkflowRuntime;
    friend class kernel::AppKernel;
    friend class ProjectRuntime;

    struct Entry final {
        kernel::ProjectId projectId;
        DocumentLifecycleState state{DocumentLifecycleState::Detached};
        std::array<std::size_t, 6U> activities{};
        std::optional<foundation::Error> error;
    };
    struct ActivityToken;

    struct CloseBlockers final {
        std::function<std::size_t(const kernel::DocumentId&)> transactions;
        std::function<std::size_t(const kernel::DocumentId&)> tasks;
        std::function<std::size_t(const kernel::DocumentId&)> workflows;
        std::function<std::size_t(const kernel::DocumentId&)> scripts;
    };

    [[nodiscard]] foundation::Result<void> configureDocument(
        kernel::ProjectId projectId,
        kernel::DocumentId documentId);
    [[nodiscard]] foundation::Result<void> adoptRecovered(
        const std::vector<state::DocumentImage>& images);
    [[nodiscard]] foundation::Result<void> adoptCatalog(
        const std::vector<persistence::DocumentCatalogRecord>& records);
    void configureCloseBlockers(CloseBlockers blockers);
    [[nodiscard]] foundation::Result<ProjectActivityLease> acquireProject(
        const kernel::DocumentId& documentId) const;
    [[nodiscard]] foundation::Result<ProjectActivityLease> acquireProjectActivity(
        const kernel::ProjectId& projectId) const;
    [[nodiscard]] foundation::Result<std::vector<kernel::DocumentId>> preflightProjectClose(
        const kernel::ProjectId& projectId) const;
    [[nodiscard]] foundation::Result<DocumentLifecycleSnapshot> closeForProject(
        const kernel::ProjectId& projectId, const kernel::DocumentId& documentId);
    [[nodiscard]] foundation::Result<DocumentActivityLease> acquireActivity(
        const kernel::DocumentId& documentId,
        DocumentActivityKind kind) const;
    void releaseActivity(
        const kernel::DocumentId& documentId,
        DocumentActivityKind kind) const noexcept;
    [[nodiscard]] foundation::Result<DocumentLifecycleSnapshot> detachImpl(
        const kernel::DocumentId& documentId,
        bool persist,
        const ProjectActivityLease* projectLease = nullptr);
    [[nodiscard]] foundation::Result<DocumentLifecycleSnapshot> openImpl(
        const kernel::DocumentId& documentId, const kernel::ProjectId* expectedProject);
    [[nodiscard]] foundation::Result<void> removeImpl(
        const kernel::DocumentId& documentId, const ProjectActivityLease& projectLease);
    [[nodiscard]] static DocumentLifecycleSnapshot snapshotOf(
        const kernel::DocumentId& documentId,
        const Entry& entry);
    void start() noexcept;
    void stop() noexcept;

    kernel::ExecutionAdmission* admission_{nullptr};
    state::DocumentStore& documents_;
    persistence::PersistenceService& persistence_;
    const state::ObjectTypeRegistry* objectTypes_;
    const platform::IAssetStore* assetStore_;
    ProjectRuntime* projects_{nullptr};
    mutable std::mutex mutex_;
    mutable std::map<kernel::DocumentId, Entry> entries_;
    CloseBlockers blockers_;
    std::atomic_bool accepting_{false};
};

[[nodiscard]] const char* documentLifecycleStateName(
    DocumentLifecycleState state) noexcept;

} // namespace lasercnc::runtime
