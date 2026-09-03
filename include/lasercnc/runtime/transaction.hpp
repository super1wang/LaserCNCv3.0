#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/version.hpp>
#include <lasercnc/messaging/domain_event.hpp>
#include <lasercnc/state/document.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace lasercnc::runtime {

class TransactionManager;
class CommandRuntime;

enum class TransactionState : std::uint8_t {
    Active,
    Failed,
    Committed,
    RolledBack
};

enum class ObjectChangeKind : std::uint8_t {
    Created,
    Updated,
    Removed
};

enum class HistoryMutationKind : std::uint8_t {
    None,
    Record,
    Barrier,
    Undo,
    Redo
};

struct HistoryMutation final {
    HistoryMutationKind kind{HistoryMutationKind::None};
    std::optional<kernel::CommandName> command;
    std::optional<foundation::Version> commandVersion;
    std::optional<kernel::TransactionId> targetTransactionId;
    std::optional<std::uint64_t> expectedCursor;

    friend bool operator==(const HistoryMutation&, const HistoryMutation&) = default;
};

struct ObjectChange final {
    ObjectChangeKind kind;
    kernel::ObjectId objectId;
    std::optional<state::ObjectRecord> before;
    std::optional<state::ObjectRecord> after;
};

struct TransactionCommit final {
    kernel::TransactionId transactionId;
    kernel::ProjectId projectId;
    kernel::DocumentId documentId;
    state::RevisionSet revisionsBefore;
    state::RevisionSet revisionsAfter;
    std::vector<ObjectChange> changes;
    std::vector<messaging::CommittedDomainEvent> events;
    HistoryMutation history;
};

struct TransactionIdempotency final {
    kernel::IdempotencyKey key;
    foundation::Value signature;
    foundation::Value result;
};

class ApplicationTransaction final {
public:
    ~ApplicationTransaction();

    ApplicationTransaction(const ApplicationTransaction&) = delete;
    ApplicationTransaction& operator=(const ApplicationTransaction&) = delete;
    ApplicationTransaction(ApplicationTransaction&&) = delete;
    ApplicationTransaction& operator=(ApplicationTransaction&&) = delete;

    [[nodiscard]] const kernel::TransactionId& id() const noexcept;
    [[nodiscard]] const kernel::ProjectId& projectId() const noexcept;
    [[nodiscard]] const kernel::DocumentId& documentId() const noexcept;
    [[nodiscard]] const state::RevisionSet& baseRevisions() const noexcept;
    [[nodiscard]] TransactionState transactionState() const noexcept;
    [[nodiscard]] const state::ObjectRegistry& stagedObjects() const noexcept;

    [[nodiscard]] foundation::Result<void> createObject(state::ObjectRecord object);
    [[nodiscard]] foundation::Result<void> replaceObjectData(
        const kernel::ObjectId& objectId,
        foundation::Value data);
    [[nodiscard]] foundation::Result<void> migrateObject(
        const kernel::ObjectId& objectId,
        foundation::Version targetVersion);
    [[nodiscard]] foundation::Result<void> replaceObjectAssets(
        const kernel::ObjectId& objectId, std::vector<state::AssetRef> assets);
    [[nodiscard]] foundation::Result<void> removeObject(const kernel::ObjectId& objectId);
    [[nodiscard]] foundation::Result<void> touchRevision(state::RevisionScope scope);
    [[nodiscard]] foundation::Result<void> collectEvent(
        messaging::PendingDomainEvent event);

    [[nodiscard]] foundation::Result<TransactionCommit> commit();
    [[nodiscard]] foundation::Result<void> rollback();

private:
    friend class CommandRuntime;
    friend class HistoryRuntime;
    friend class TransactionManager;

    static constexpr std::size_t revisionScopeCount = 6U;

    ApplicationTransaction(
        TransactionManager& manager,
        kernel::TransactionId transactionId,
        state::Document baseDocument);

    [[nodiscard]] foundation::Result<void> ensureActive() const;
    [[nodiscard]] foundation::Result<void> restoreObject(state::ObjectRecord object);
    [[nodiscard]] foundation::Result<void> attachIdempotency(
        TransactionIdempotency idempotency);
    [[nodiscard]] foundation::Result<void> attachHistoryMutation(
        HistoryMutation mutation);
    [[nodiscard]] foundation::Result<void> fail(foundation::Error error);
    void markDocumentChanged() noexcept;
    [[nodiscard]] std::vector<ObjectChange> buildChanges() const;
    void release(TransactionState terminalState) noexcept;

    TransactionManager* manager_;
    kernel::TransactionId transactionId_;
    state::Document baseDocument_;
    state::ObjectRegistry stagedObjects_;
    TransactionState state_{TransactionState::Active};
    std::optional<foundation::Error> failure_;
    std::array<bool, revisionScopeCount> affectedScopes_ {};
    std::vector<messaging::PendingDomainEvent> pendingEvents_;
    std::optional<TransactionIdempotency> idempotency_;
    HistoryMutation historyMutation_;
};

} // namespace lasercnc::runtime
