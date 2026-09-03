#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/kernel/identifiers.hpp>
#include <lasercnc/runtime/transaction.hpp>
#include <lasercnc/state/document_store.hpp>
#include <lasercnc/state/revision.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <vector>

namespace lasercnc::persistence {
class PersistenceService;
}

namespace lasercnc::runtime {

class TransactionManager final {
public:
    explicit TransactionManager(
        state::DocumentStore& documents,
        persistence::PersistenceService* persistence = nullptr) noexcept;

    TransactionManager(const TransactionManager&) = delete;
    TransactionManager& operator=(const TransactionManager&) = delete;

    [[nodiscard]] foundation::Result<std::unique_ptr<ApplicationTransaction>> begin(
        kernel::TransactionId transactionId,
        const kernel::DocumentId& documentId,
        std::span<const state::RevisionPrecondition> preconditions = {});
    [[nodiscard]] std::size_t activeTransactionCount() const;

private:
    friend class ApplicationTransaction;

    [[nodiscard]] foundation::Result<TransactionCommit> commit(
        ApplicationTransaction& transaction,
        std::vector<ObjectChange> changes);
    void release(const kernel::TransactionId& transactionId) noexcept;

    state::DocumentStore& documents_;
    persistence::PersistenceService* persistence_;
    std::mutex commitMutex_;
    mutable std::mutex activeMutex_;
    std::set<kernel::TransactionId> activeTransactions_;
};

} // namespace lasercnc::runtime
