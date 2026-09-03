#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/serialization.hpp>
#include <lasercnc/kernel/identifiers.hpp>
#include <lasercnc/platform/hash_service.hpp>
#include <lasercnc/platform/persistence_backend.hpp>
#include <lasercnc/runtime/transaction.hpp>
#include <lasercnc/state/revision.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
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

class PersistenceService final {
public:
    [[nodiscard]] foundation::Result<void> configure(
        std::unique_ptr<platform::IPersistenceBackend> backend,
        std::shared_ptr<foundation::IValueSerializer> serializer,
        std::shared_ptr<platform::IHashService> hashes);
    [[nodiscard]] foundation::Result<void> initialize();
    [[nodiscard]] foundation::Result<JournalRecord> append(
        const runtime::TransactionCommit& commit);
    [[nodiscard]] foundation::Result<std::vector<JournalRecord>> journalAfter(
        const kernel::DocumentId& documentId,
        std::uint64_t sequence) const;

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
    bool initialized_{false};
    bool frozen_{false};
};

} // namespace lasercnc::persistence
