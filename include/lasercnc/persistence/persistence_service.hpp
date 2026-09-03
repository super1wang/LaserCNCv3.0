#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/serialization.hpp>
#include <lasercnc/kernel/identifiers.hpp>
#include <lasercnc/platform/hash_service.hpp>
#include <lasercnc/platform/persistence_backend.hpp>
#include <lasercnc/platform/snapshot_store.hpp>
#include <lasercnc/runtime/transaction.hpp>
#include <lasercnc/state/revision.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
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

struct RecoveryReport final {
    std::vector<state::DocumentImage> documents;
    std::uint64_t latestJournalSequence{0U};
    std::size_t journalRecordsReplayed{0U};
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
        const runtime::TransactionCommit& commit);
    [[nodiscard]] foundation::Result<std::vector<JournalRecord>> journalAfter(
        const kernel::DocumentId& documentId,
        std::uint64_t sequence) const;
    [[nodiscard]] foundation::Result<SnapshotRecord> captureSnapshot(
        kernel::SnapshotId snapshotId,
        const state::Document& document);
    [[nodiscard]] foundation::Result<std::optional<SnapshotRecord>> latestSnapshot(
        const kernel::DocumentId& documentId) const;
    [[nodiscard]] foundation::Result<RecoveryReport> recover() const;

    [[nodiscard]] bool configured() const;
    [[nodiscard]] bool ready() const;
    [[nodiscard]] bool frozen() const;

private:
    friend class kernel::AppKernel;

    void freeze();

    mutable std::mutex mutex_;
    std::unique_ptr<platform::IPersistenceBackend> backend_;
    std::shared_ptr<foundation::IValueSerializer> serializer_;
    std::shared_ptr<platform::IHashService> hashes_;
    std::unique_ptr<platform::ISnapshotStore> snapshotStore_;
    bool initialized_{false};
    bool frozen_{false};
};

} // namespace lasercnc::persistence
