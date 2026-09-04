#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/serialization.hpp>
#include <lasercnc/kernel/identifiers.hpp>
#include <lasercnc/platform/hash_service.hpp>
#include <lasercnc/platform/persistence_backend.hpp>
#include <lasercnc/platform/snapshot_store.hpp>
#include <lasercnc/runtime/transaction.hpp>
#include <lasercnc/runtime/task.hpp>
#include <lasercnc/runtime/execution_contract.hpp>
#include <lasercnc/runtime/workflow.hpp>
#include <lasercnc/observability/diagnostics_service.hpp>
#include <lasercnc/state/revision.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lasercnc::kernel {
class AppKernel;
}

namespace lasercnc::persistence {

struct JournalRecord final {
    std::uint64_t sequence{0U};
    kernel::TransactionId transactionId;
    kernel::ProjectId projectId;
    kernel::DocumentId documentId;
    state::RevisionSet revisionsBefore;
    state::RevisionSet revisionsAfter;
    std::string payload;
    kernel::ContentDigest digest;
    std::chrono::system_clock::time_point committedAt;
};

struct SnapshotRecord final {
    kernel::SnapshotId snapshotId;
    kernel::ProjectId projectId;
    kernel::DocumentId documentId;
    state::RevisionSet revisions;
    std::uint64_t journalSequence{0U};
    std::string payload;
    kernel::ContentDigest digest;
    std::chrono::system_clock::time_point createdAt;
};

enum class DocumentPersistenceState : std::uint8_t {
    Detached,
    Opening,
    Open,
    Closing,
    Failed,
    Removed
};

struct DocumentCatalogRecord final {
    kernel::ProjectId projectId;
    kernel::DocumentId documentId;
    DocumentPersistenceState state{DocumentPersistenceState::Detached};
    bool interruptedTransition{false};
    std::chrono::system_clock::time_point updatedAt;
};

enum class ProjectPersistenceState : std::uint8_t {
    Closed,
    Opening,
    Open,
    Closing,
    Failed
};

struct ProjectCatalogRecord final {
    kernel::ProjectId projectId;
    ProjectPersistenceState state{ProjectPersistenceState::Closed};
    bool interruptedTransition{false};
    std::chrono::system_clock::time_point updatedAt;
};

struct RecoveryReport final {
    std::vector<state::DocumentImage> documents;
    std::vector<runtime::TransactionCommit> historyCommits;
    std::uint64_t latestJournalSequence{0U};
    std::size_t journalRecordsReplayed{0U};
};

enum class IdempotencyClaimDisposition : std::uint8_t {
    Acquired,
    Replayed
};

struct IdempotencyReplay final {
    foundation::Value result;
    std::optional<runtime::TransactionCommit> commit;
    std::optional<kernel::TaskId> taskId;
};

struct IdempotencyClaim final {
    IdempotencyClaimDisposition disposition{IdempotencyClaimDisposition::Acquired};
    std::optional<IdempotencyReplay> replay;
};

enum class ExternalEffectClaimDisposition : std::uint8_t {
    Acquired,
    Replayed
};

struct ExternalEffectClaim final {
    ExternalEffectClaimDisposition disposition{ExternalEffectClaimDisposition::Acquired};
    std::optional<foundation::Value> replay;
    bool resumed{false};
};

struct ExternalEffectRecord final {
    kernel::IdempotencyKey idempotencyKey;
    runtime::ReplayPolicy replayPolicy{runtime::ReplayPolicy::Never};
    runtime::ExternalEffectState state{runtime::ExternalEffectState::Executing};
    std::optional<foundation::Value> outcome;
    std::chrono::system_clock::time_point startedAt;
    std::chrono::system_clock::time_point updatedAt;
};

struct WorkflowCheckpoint final {
    runtime::WorkflowRequest request;
    kernel::ContentDigest definitionDigest;
    runtime::WorkflowSnapshot snapshot;
    std::vector<kernel::WorkflowStepId> completionOrder;
    std::chrono::system_clock::time_point updatedAt;
};

class PersistenceService final {
public:
    [[nodiscard]] foundation::Result<void> configure(
        std::unique_ptr<platform::IPersistenceBackend> backend,
        std::shared_ptr<foundation::IValueSerializer> serializer,
        std::shared_ptr<platform::IHashService> hashes,
        std::unique_ptr<platform::ISnapshotStore> snapshotStore = nullptr);
    [[nodiscard]] foundation::Result<void> initialize();
    [[nodiscard]] foundation::Result<JournalRecord> append(
        const runtime::TransactionCommit& commit,
        const std::optional<runtime::TransactionIdempotency>& idempotency = std::nullopt);
    [[nodiscard]] foundation::Result<std::vector<JournalRecord>> journalAfter(
        const kernel::DocumentId& documentId,
        std::uint64_t sequence) const;
    [[nodiscard]] foundation::Result<SnapshotRecord> captureSnapshot(
        kernel::SnapshotId snapshotId,
        const state::Document& document);
    [[nodiscard]] foundation::Result<std::optional<SnapshotRecord>> latestSnapshot(
        const kernel::DocumentId& documentId) const;
    [[nodiscard]] foundation::Result<RecoveryReport> recover() const;
    [[nodiscard]] foundation::Result<void> saveDocumentLifecycle(
        const kernel::ProjectId& projectId,
        const kernel::DocumentId& documentId,
        DocumentPersistenceState state);
    [[nodiscard]] foundation::Result<void> removeDocumentLifecycle(
        const kernel::ProjectId& projectId,
        const kernel::DocumentId& documentId);
    [[nodiscard]] foundation::Result<std::vector<DocumentCatalogRecord>>
        documentCatalog() const;
    [[nodiscard]] foundation::Result<bool> projectCatalogMigrationPending() const;
    [[nodiscard]] foundation::Result<void> completeProjectCatalogMigration(
        std::span<const kernel::ProjectId> verifiedLegacyProjects);
    [[nodiscard]] foundation::Result<void> saveProjectLifecycle(
        const kernel::ProjectId& projectId, ProjectPersistenceState state);
    [[nodiscard]] foundation::Result<std::vector<ProjectCatalogRecord>> projectCatalog() const;
    [[nodiscard]] foundation::Result<IdempotencyClaim> claimCommand(
        const kernel::IdempotencyKey& key,
        const foundation::Value& signature);
    [[nodiscard]] foundation::Result<void> releaseCommandClaim(
        const kernel::IdempotencyKey& key,
        const foundation::Value& signature);
    [[nodiscard]] foundation::Result<ExternalEffectClaim> claimExternalEffect(
        const kernel::IdempotencyKey& key,
        const foundation::Value& signature,
        runtime::ReplayPolicy replayPolicy);
    [[nodiscard]] foundation::Result<void> completeExternalEffect(
        const kernel::IdempotencyKey& key,
        const foundation::Value& signature,
        const foundation::Value& outcome);
    [[nodiscard]] foundation::Result<runtime::RecoveryDisposition> interruptExternalEffect(
        const kernel::IdempotencyKey& key,
        const foundation::Value& signature);
    [[nodiscard]] foundation::Result<std::optional<ExternalEffectRecord>> externalEffect(
        const kernel::IdempotencyKey& key) const;
    [[nodiscard]] foundation::Result<void> acceptTask(
        const runtime::TaskRequest& request,
        const std::optional<state::RevisionSet>& sourceRevisions,
        const std::optional<runtime::TransactionIdempotency>& commandIdempotency = std::nullopt);
    [[nodiscard]] foundation::Result<void> recordTaskTerminal(
        const runtime::TaskSnapshot& snapshot);
    [[nodiscard]] foundation::Result<std::optional<runtime::TaskSnapshot>> taskHistory(
        const kernel::TaskId& taskId) const;
    [[nodiscard]] foundation::Result<void> recordDiagnostic(
        const observability::DiagnosticReport& report);
    [[nodiscard]] foundation::Result<std::vector<observability::DiagnosticReport>> diagnosticHistory(
        const kernel::DiagnosticId& diagnosticId) const;
    [[nodiscard]] foundation::Result<std::vector<observability::DiagnosticReport>> latestDiagnostics() const;
    [[nodiscard]] foundation::Result<kernel::ContentDigest> workflowDefinitionDigest(
        const runtime::WorkflowDefinition& definition) const;
    [[nodiscard]] foundation::Result<void> saveWorkflowCheckpoint(
        const runtime::WorkflowRequest& request,
        const runtime::WorkflowDefinition& definition,
        const runtime::WorkflowSnapshot& snapshot,
        const std::vector<kernel::WorkflowStepId>& completionOrder);
    [[nodiscard]] foundation::Result<std::optional<WorkflowCheckpoint>> workflowCheckpoint(
        const kernel::WorkflowId& workflowId) const;
    [[nodiscard]] foundation::Result<std::vector<WorkflowCheckpoint>> workflowCheckpoints() const;

    [[nodiscard]] bool configured() const;
    [[nodiscard]] bool ready() const;
    [[nodiscard]] bool frozen() const;

private:
    friend class kernel::AppKernel;

    void freeze();
    [[nodiscard]] foundation::Result<void> completeCommandInOpenTransaction(
        const runtime::TransactionCommit& commit,
        const runtime::TransactionIdempotency& idempotency);
    [[nodiscard]] foundation::Result<void> completeTaskCommandInOpenTransaction(
        const runtime::TaskRequest& request,
        const runtime::TransactionIdempotency& idempotency);
    [[nodiscard]] foundation::Result<runtime::TransactionCommit> loadCommitUnlocked(
        const kernel::TransactionId& transactionId) const;

    mutable std::mutex mutex_;
    std::unique_ptr<platform::IPersistenceBackend> backend_;
    std::shared_ptr<foundation::IValueSerializer> serializer_;
    std::shared_ptr<platform::IHashService> hashes_;
    std::unique_ptr<platform::ISnapshotStore> snapshotStore_;
    bool initialized_{false};
    bool frozen_{false};
};

} // namespace lasercnc::persistence
