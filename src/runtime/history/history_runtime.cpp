#include <lasercnc/runtime/history_runtime.hpp>

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/foundation/schema.hpp>
#include <lasercnc/runtime/command.hpp>
#include <lasercnc/runtime/command_registry.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace lasercnc::runtime {
namespace {

foundation::Error historyError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const kernel::DocumentId& documentId)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {foundation::Value::Object {
            {"documentId", foundation::Value {std::string(documentId.value())}},
        }});
}

template <typename Id>
foundation::Result<Id> makeId(std::string value)
{
    return Id::create(std::move(value));
}

foundation::Result<kernel::HistoryEntryId> entryId(
    const kernel::TransactionId& transactionId)
{
    return makeId<kernel::HistoryEntryId>(
        "history." + std::string(transactionId.value()));
}

bool hasCommitEffect(const TransactionCommit& commit) noexcept
{
    return commit.revisionsBefore != commit.revisionsAfter
        || !commit.changes.empty() || !commit.events.empty();
}

foundation::Result<void> verifyCurrent(
    const state::ObjectRegistry& objects,
    const ObjectChange& change,
    bool undo,
    const kernel::DocumentId& documentId)
{
    const auto* current = objects.find(change.objectId);
    const auto& expected = undo ? change.after : change.before;
    if((expected.has_value() && (current == nullptr || *current != *expected))
       || (!expected.has_value() && current != nullptr)) {
        return foundation::Result<void>::failure(historyError(
            "History.ObjectStateConflict",
            foundation::ErrorCategory::Conflict,
            "The document no longer matches the selected history entry",
            documentId));
    }
    return foundation::Result<void>::success();
}

foundation::Result<void> applyChange(
    ApplicationTransaction& transaction,
    const ObjectChange& change,
    bool undo)
{
    auto verified = verifyCurrent(
        transaction.stagedObjects(), change, undo, transaction.documentId());
    if(!verified) {
        return verified;
    }
    const auto& target = undo ? change.before : change.after;
    const auto& source = undo ? change.after : change.before;
    if(!source.has_value() && target.has_value()) {
        return transaction.createObject(*target);
    }
    if(source.has_value() && !target.has_value()) {
        return transaction.removeObject(change.objectId);
    }
    if(source.has_value() && target.has_value()) {
        if(source->type != target->type) {
            return foundation::Result<void>::failure(historyError(
                "History.ObjectTypeChanged",
                foundation::ErrorCategory::Infrastructure,
                "A history update attempts to change an immutable object type",
                transaction.documentId()));
        }
        return transaction.replaceObjectData(change.objectId, target->data);
    }
    return foundation::Result<void>::failure(historyError(
        "History.InvalidObjectChange",
        foundation::ErrorCategory::Infrastructure,
        "A history entry contains an empty object change",
        transaction.documentId()));
}

constexpr std::array revisionScopes {
    state::RevisionScope::Project,
    state::RevisionScope::Document,
    state::RevisionScope::Geometry,
    state::RevisionScope::Cam,
    state::RevisionScope::MachineContext,
    state::RevisionScope::Environment,
};

foundation::Result<void> touchEntryScopes(
    ApplicationTransaction& transaction,
    const HistoryEntry& entry)
{
    for(const auto scope : revisionScopes) {
        if(entry.revisionsBefore.at(scope) == entry.revisionsAfter.at(scope)) {
            continue;
        }
        auto touched = transaction.touchRevision(scope);
        if(!touched) {
            return touched;
        }
    }
    return foundation::Result<void>::success();
}

foundation::Result<foundation::Schema> schema(
    const char* id,
    foundation::SchemaKind kind)
{
    auto schemaId = foundation::SchemaId::create(id);
    if(!schemaId) {
        return foundation::Result<foundation::Schema>::failure(
            std::move(schemaId).error());
    }
    return foundation::Schema::create(
        std::move(schemaId).value(), foundation::Version {1U, 0U, 0U}, kind);
}

} // namespace

class HistoryCommandHandler final : public ICommandHandler {
public:
    HistoryCommandHandler(HistoryRuntime& history, bool undo) noexcept
        : history_(&history), undo_(undo)
    {
    }

    foundation::Result<foundation::Value> execute(
        const CommandRequest&,
        ApplicationTransaction& transaction) override
    {
        auto prepared = undo_ ? history_->prepareUndo(transaction)
                              : history_->prepareRedo(transaction);
        if(!prepared) {
            return foundation::Result<foundation::Value>::failure(
                std::move(prepared).error());
        }
        return foundation::Result<foundation::Value>::success(
            foundation::Value {foundation::Value::Object {
                {"operation", foundation::Value {undo_ ? "undo" : "redo"}},
            }});
    }

private:
    HistoryRuntime* history_;
    bool undo_;
};

HistoryRuntime::HistoryRuntime(const state::DocumentStore& documents) noexcept
    : documents_(documents)
{
}

foundation::Result<DocumentHistorySnapshot> HistoryRuntime::snapshot(
    const kernel::DocumentId& documentId) const
{
    std::lock_guard lock(mutex_);
    const auto found = histories_.find(documentId);
    if(found != histories_.end()) {
        return foundation::Result<DocumentHistorySnapshot>::success(
            DocumentHistorySnapshot {
                found->second.projectId,
                found->second.documentId,
                found->second.cursor,
                found->second.entries,
                found->second.barrier});
    }
    auto document = documents_.snapshot(documentId);
    if(!document) {
        return foundation::Result<DocumentHistorySnapshot>::failure(
            std::move(document).error());
    }
    return foundation::Result<DocumentHistorySnapshot>::success(
        DocumentHistorySnapshot {
            document.value().projectId(), documentId, HistoryCursor {}, {}, std::nullopt});
}

foundation::Result<void> HistoryRuntime::registerCommands(CommandRegistry& registry)
{
    auto arguments = schema("kernel.history.arguments", foundation::SchemaKind::Object);
    auto result = schema("kernel.history.result", foundation::SchemaKind::Object);
    auto capability = kernel::CapabilityId::create("kernel.history.edit");
    auto undoName = kernel::CommandName::create("edit.undo");
    auto redoName = kernel::CommandName::create("edit.redo");
    if(!arguments || !result || !capability || !undoName || !redoName) {
        return foundation::Result<void>::failure(foundation::makeError(
            "History.BuiltinContractInvalid",
            foundation::ErrorCategory::Internal,
            "The built-in history command contract is invalid"));
    }
    const auto descriptor = [&](kernel::CommandName name) {
        return CommandDescriptor {
            std::move(name),
            foundation::Version {1U, 0U, 0U},
            arguments.value(),
            result.value(),
            ExecutionMode::Synchronous,
            SideEffectLevel::DocumentWrite,
            capability.value(),
            false,
            true,
            false,
            ContractStatus::Active,
            ExecutionScope::Document};
    };
    auto undo = registry.registerHandler(
        descriptor(std::move(undoName).value()),
        std::make_shared<HistoryCommandHandler>(*this, true));
    if(!undo) {
        return undo;
    }
    return registry.registerHandler(
        descriptor(std::move(redoName).value()),
        std::make_shared<HistoryCommandHandler>(*this, false));
}

foundation::Result<void> HistoryRuntime::prepareUndo(
    ApplicationTransaction& transaction)
{
    std::optional<HistoryEntry> selected;
    std::uint64_t selectedCursor = 0U;
    {
        std::lock_guard lock(mutex_);
        const auto found = histories_.find(transaction.documentId());
        if(found == histories_.end() || !found->second.cursor.canUndo()) {
            return foundation::Result<void>::failure(historyError(
                "History.UndoUnavailable",
                foundation::ErrorCategory::Conflict,
                "The document has no undoable history entry",
                transaction.documentId()));
        }
        selected = found->second.entries[found->second.cursor.position - 1U];
        selectedCursor = static_cast<std::uint64_t>(found->second.cursor.position);
    }
    const auto& entry = *selected;
    for(auto change = entry.changes.rbegin(); change != entry.changes.rend(); ++change) {
        auto applied = applyChange(transaction, *change, true);
        if(!applied) {
            return applied;
        }
    }
    auto touched = touchEntryScopes(transaction, entry);
    if(!touched) {
        return touched;
    }
    return transaction.attachHistoryMutation(HistoryMutation {
        HistoryMutationKind::Undo,
        std::nullopt,
        std::nullopt,
        entry.transactionId,
        selectedCursor});
}

foundation::Result<void> HistoryRuntime::prepareRedo(
    ApplicationTransaction& transaction)
{
    std::optional<HistoryEntry> selected;
    std::uint64_t selectedCursor = 0U;
    {
        std::lock_guard lock(mutex_);
        const auto found = histories_.find(transaction.documentId());
        if(found == histories_.end() || !found->second.cursor.canRedo()) {
            return foundation::Result<void>::failure(historyError(
                "History.RedoUnavailable",
                foundation::ErrorCategory::Conflict,
                "The document has no redoable history entry",
                transaction.documentId()));
        }
        selected = found->second.entries[found->second.cursor.position];
        selectedCursor = static_cast<std::uint64_t>(found->second.cursor.position);
    }
    const auto& entry = *selected;
    for(const auto& change : entry.changes) {
        auto applied = applyChange(transaction, change, false);
        if(!applied) {
            return applied;
        }
    }
    auto touched = touchEntryScopes(transaction, entry);
    if(!touched) {
        return touched;
    }
    return transaction.attachHistoryMutation(HistoryMutation {
        HistoryMutationKind::Redo,
        std::nullopt,
        std::nullopt,
        entry.transactionId,
        selectedCursor});
}

foundation::Result<HistoryRuntime::DocumentHistory> HistoryRuntime::applyCommit(
    const TransactionCommit& commit,
    std::optional<DocumentHistory> current) const
{
    DocumentHistory next = current.has_value()
        ? std::move(*current)
        : DocumentHistory {commit.projectId, commit.documentId, {}, {}, std::nullopt};
    if(next.projectId != commit.projectId || next.documentId != commit.documentId) {
        return foundation::Result<DocumentHistory>::failure(historyError(
            "History.DocumentOwnershipChanged",
            foundation::ErrorCategory::Infrastructure,
            "History material changes document ownership",
            commit.documentId));
    }
    const auto& mutation = commit.history;
    switch(mutation.kind) {
    case HistoryMutationKind::None:
        if(hasCommitEffect(commit)) {
            return foundation::Result<DocumentHistory>::failure(historyError(
                "History.MissingBarrier",
                foundation::ErrorCategory::Infrastructure,
                "An effective transaction is missing its history barrier",
                commit.documentId));
        }
        return foundation::Result<DocumentHistory>::success(std::move(next));
    case HistoryMutationKind::Barrier:
        if(!hasCommitEffect(commit)) {
            return foundation::Result<DocumentHistory>::failure(historyError(
                "History.EmptyBarrier",
                foundation::ErrorCategory::Infrastructure,
                "An empty transaction cannot create a history barrier",
                commit.documentId));
        }
        next.entries.clear();
        next.cursor = {};
        next.barrier = UndoBarrier {commit.transactionId, commit.revisionsAfter};
        return foundation::Result<DocumentHistory>::success(std::move(next));
    case HistoryMutationKind::Record: {
        if(!mutation.command.has_value() || !mutation.commandVersion.has_value()
           || !hasCommitEffect(commit)) {
            return foundation::Result<DocumentHistory>::failure(historyError(
                "History.InvalidRecord",
                foundation::ErrorCategory::Infrastructure,
                "A history record is missing its command identity or transaction effect",
                commit.documentId));
        }
        auto id = entryId(commit.transactionId);
        if(!id) {
            return foundation::Result<DocumentHistory>::failure(std::move(id).error());
        }
        next.entries.erase(
            next.entries.begin() + static_cast<std::ptrdiff_t>(next.cursor.position),
            next.entries.end());
        next.entries.push_back(HistoryEntry {
            std::move(id).value(),
            commit.transactionId,
            commit.projectId,
            commit.documentId,
            *mutation.command,
            *mutation.commandVersion,
            commit.revisionsBefore,
            commit.revisionsAfter,
            commit.changes});
        next.cursor.position = next.entries.size();
        next.cursor.extent = next.entries.size();
        return foundation::Result<DocumentHistory>::success(std::move(next));
    }
    case HistoryMutationKind::Undo:
        if(!mutation.targetTransactionId.has_value() || !mutation.expectedCursor.has_value()
           || *mutation.expectedCursor != next.cursor.position
           || !next.cursor.canUndo()
           || next.entries[next.cursor.position - 1U].transactionId
                != *mutation.targetTransactionId) {
            return foundation::Result<DocumentHistory>::failure(historyError(
                "History.UndoCursorConflict",
                foundation::ErrorCategory::Conflict,
                "The undo target no longer matches the history cursor",
                commit.documentId));
        }
        --next.cursor.position;
        return foundation::Result<DocumentHistory>::success(std::move(next));
    case HistoryMutationKind::Redo:
        if(!mutation.targetTransactionId.has_value() || !mutation.expectedCursor.has_value()
           || *mutation.expectedCursor != next.cursor.position
           || !next.cursor.canRedo()
           || next.entries[next.cursor.position].transactionId
                != *mutation.targetTransactionId) {
            return foundation::Result<DocumentHistory>::failure(historyError(
                "History.RedoCursorConflict",
                foundation::ErrorCategory::Conflict,
                "The redo target no longer matches the history cursor",
                commit.documentId));
        }
        ++next.cursor.position;
        return foundation::Result<DocumentHistory>::success(std::move(next));
    }
    return foundation::Result<DocumentHistory>::failure(historyError(
        "History.InvalidMutation",
        foundation::ErrorCategory::Infrastructure,
        "A transaction contains an unknown history mutation",
        commit.documentId));
}

foundation::Result<HistoryRuntime::DocumentHistory> HistoryRuntime::prepareCommit(
    TransactionCommit& commit)
{
    const bool effective = hasCommitEffect(commit);
    if(commit.history.kind == HistoryMutationKind::Record && !effective) {
        commit.history = {};
    } else if(commit.history.kind == HistoryMutationKind::None && effective) {
        commit.history.kind = HistoryMutationKind::Barrier;
    }
    std::lock_guard lock(mutex_);
    auto found = histories_.find(commit.documentId);
    if(found == histories_.end()) {
        const auto inserted = histories_.emplace(
            commit.documentId,
            DocumentHistory {
                commit.projectId, commit.documentId, {}, {}, std::nullopt});
        found = inserted.first;
    }
    return applyCommit(commit, found->second);
}

void HistoryRuntime::install(DocumentHistory history) noexcept
{
    std::lock_guard lock(mutex_);
    const auto found = histories_.find(history.documentId);
    if(found == histories_.end()) {
        std::terminate();
    }
    found->second = std::move(history);
}

foundation::Result<void> HistoryRuntime::restore(
    std::span<const TransactionCommit> commits)
{
    try {
        std::map<kernel::DocumentId, DocumentHistory> restored;
        for(const auto& commit : commits) {
            const auto found = restored.find(commit.documentId);
            std::optional<DocumentHistory> current;
            if(found != restored.end()) {
                current = found->second;
            }
            auto applied = applyCommit(commit, std::move(current));
            if(!applied) {
                return foundation::Result<void>::failure(std::move(applied).error());
            }
            restored.insert_or_assign(commit.documentId, std::move(applied).value());
        }
        std::lock_guard lock(mutex_);
        histories_.swap(restored);
        return foundation::Result<void>::success();
    } catch(const std::exception& exception) {
        return foundation::Result<void>::failure(foundation::makeError(
            "History.RestoreFailed",
            foundation::ErrorCategory::Internal,
            "History recovery failed unexpectedly",
            foundation::Value {foundation::Value::Object {
                {"reason", foundation::Value {exception.what()}},
            }}));
    } catch(...) {
        return foundation::Result<void>::failure(foundation::makeError(
            "History.RestoreFailed",
            foundation::ErrorCategory::Internal,
            "History recovery failed unexpectedly"));
    }
}

} // namespace lasercnc::runtime
