#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/kernel/identifiers.hpp>
#include <lasercnc/platform/asset_store.hpp>
#include <lasercnc/runtime/transaction.hpp>
#include <lasercnc/state/document_store.hpp>
#include <lasercnc/state/object_type_registry.hpp>
#include <lasercnc/state/revision.hpp>

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <vector>

namespace lasercnc::persistence {
class PersistenceService;
}

namespace lasercnc::kernel {
class AppKernel;
}

namespace lasercnc::runtime {

class HistoryRuntime;

class TransactionManager final {
public:
    explicit TransactionManager(
        state::DocumentStore& documents,
        persistence::PersistenceService* persistence = nullptr,
        DocumentRuntime* documentRuntime = nullptr,
        HistoryRuntime* historyRuntime = nullptr,
        const state::ObjectTypeRegistry* objectTypes = nullptr,
        const platform::IAssetStore* assetStore = nullptr) noexcept;

    TransactionManager(const TransactionManager&) = delete;
    TransactionManager& operator=(const TransactionManager&) = delete;

    [[nodiscard]] foundation::Result<std::unique_ptr<ApplicationTransaction>> begin(
        kernel::TransactionId transactionId,
        const kernel::DocumentId& documentId,
        std::span<const state::RevisionPrecondition> preconditions = {});
    [[nodiscard]] std::size_t activeTransactionCount() const;
    [[nodiscard]] std::size_t activeTransactionCount(
        const kernel::DocumentId& documentId) const;

private:
    friend class ApplicationTransaction;
    friend class kernel::AppKernel;

    [[nodiscard]] foundation::Result<TransactionCommit> commit(
        ApplicationTransaction& transaction,
        std::vector<ObjectChange> changes);
    void release(const kernel::TransactionId& transactionId) noexcept;

    state::DocumentStore& documents_;
    persistence::PersistenceService* persistence_;
    DocumentRuntime* documentRuntime_;
    HistoryRuntime* historyRuntime_;
    const state::ObjectTypeRegistry* objectTypes_;
    const platform::IAssetStore* assetStore_;
    std::mutex commitMutex_;
    mutable std::mutex activeMutex_;
    std::map<kernel::TransactionId, kernel::DocumentId> activeTransactions_;
};

} // namespace lasercnc::runtime
