#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/version.hpp>
#include <lasercnc/kernel/identifiers.hpp>
#include <lasercnc/runtime/transaction.hpp>
#include <lasercnc/state/revision.hpp>
#include <lasercnc/state/document_store.hpp>

#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace lasercnc::kernel {
class AppKernel;
}

namespace lasercnc::runtime {

class ApplicationTransaction;
class CommandRegistry;
class HistoryCommandHandler;
class TransactionManager;

struct HistoryEntry final {
    kernel::HistoryEntryId entryId;
    kernel::TransactionId transactionId;
    kernel::ProjectId projectId;
    kernel::DocumentId documentId;
    kernel::CommandName command;
    foundation::Version commandVersion;
    state::RevisionSet revisionsBefore;
    state::RevisionSet revisionsAfter;
    std::vector<ObjectChange> changes;
};

struct HistoryCursor final {
    std::size_t position{0U};
    std::size_t extent{0U};

    [[nodiscard]] bool canUndo() const noexcept { return position != 0U; }
    [[nodiscard]] bool canRedo() const noexcept { return position < extent; }

    friend bool operator==(const HistoryCursor&, const HistoryCursor&) = default;
};

struct UndoBarrier final {
    kernel::TransactionId transactionId;
    state::RevisionSet revisionsAfter;
};

struct DocumentHistorySnapshot final {
    kernel::ProjectId projectId;
    kernel::DocumentId documentId;
    HistoryCursor cursor;
    std::vector<HistoryEntry> entries;
    std::optional<UndoBarrier> barrier;
};

class HistoryRuntime final {
public:
    explicit HistoryRuntime(const state::DocumentStore& documents) noexcept;

    [[nodiscard]] foundation::Result<DocumentHistorySnapshot> snapshot(
        const kernel::DocumentId& documentId) const;
    [[nodiscard]] foundation::Result<void> restore(
        std::span<const TransactionCommit> commits);

private:
    friend class kernel::AppKernel;
    friend class HistoryCommandHandler;
    friend class TransactionManager;

    struct DocumentHistory final {
        kernel::ProjectId projectId;
        kernel::DocumentId documentId;
        HistoryCursor cursor;
        std::vector<HistoryEntry> entries;
        std::optional<UndoBarrier> barrier;
    };

    [[nodiscard]] foundation::Result<void> registerCommands(CommandRegistry& registry);
    [[nodiscard]] static foundation::Result<void> applyChange(
        ApplicationTransaction& transaction, const ObjectChange& change, bool undo);
    [[nodiscard]] foundation::Result<void> prepareUndo(
        ApplicationTransaction& transaction);
    [[nodiscard]] foundation::Result<void> prepareRedo(
        ApplicationTransaction& transaction);
    [[nodiscard]] foundation::Result<DocumentHistory> prepareCommit(
        TransactionCommit& commit);
    void install(DocumentHistory history) noexcept;

    [[nodiscard]] foundation::Result<DocumentHistory> applyCommit(
        const TransactionCommit& commit,
        std::optional<DocumentHistory> current) const;

    mutable std::mutex mutex_;
    const state::DocumentStore& documents_;
    std::map<kernel::DocumentId, DocumentHistory> histories_;
};

} // namespace lasercnc::runtime
