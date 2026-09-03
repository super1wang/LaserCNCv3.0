#include <lasercnc/runtime/transaction_manager.hpp>

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/persistence/persistence_service.hpp>
#include <lasercnc/runtime/document_runtime.hpp>
#include <lasercnc/runtime/history_runtime.hpp>

#include <array>
#include <exception>
#include <mutex>
#include <string>
#include <utility>

namespace lasercnc::runtime {
namespace {

foundation::Error managerError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const kernel::TransactionId& transactionId)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {foundation::Value::Object {
            {"transactionId", foundation::Value {std::string(transactionId.value())}},
        }});
}

constexpr std::array allRevisionScopes {
    state::RevisionScope::Project,
    state::RevisionScope::Document,
    state::RevisionScope::Geometry,
    state::RevisionScope::Cam,
    state::RevisionScope::MachineContext,
    state::RevisionScope::Environment,
};

} // namespace

TransactionManager::TransactionManager(
    state::DocumentStore& documents,
    persistence::PersistenceService* persistence,
    DocumentRuntime* documentRuntime,
    HistoryRuntime* historyRuntime,
    const state::ObjectTypeRegistry* objectTypes) noexcept
    : documents_(documents),
      persistence_(persistence),
      documentRuntime_(documentRuntime),
      historyRuntime_(historyRuntime),
      objectTypes_(objectTypes)
{
}

foundation::Result<std::unique_ptr<ApplicationTransaction>> TransactionManager::begin(
    kernel::TransactionId transactionId,
    const kernel::DocumentId& documentId,
    std::span<const state::RevisionPrecondition> preconditions)
{
    std::optional<DocumentActivityLease> activity;
    if(documentRuntime_ != nullptr) {
        auto admitted = documentRuntime_->acquireActivity(
            documentId, DocumentActivityKind::Transaction);
        if(!admitted) {
            return foundation::Result<std::unique_ptr<ApplicationTransaction>>::failure(
                std::move(admitted).error());
        }
        activity.emplace(std::move(admitted).value());
    }
    try {
        {
            std::lock_guard lock(activeMutex_);
            const auto [unused, inserted] = activeTransactions_.emplace(
                transactionId, documentId);
            static_cast<void>(unused);
            if(!inserted) {
                return foundation::Result<std::unique_ptr<ApplicationTransaction>>::failure(managerError(
                    "Transaction.AlreadyActive",
                    foundation::ErrorCategory::Conflict,
                    "A transaction with the same stable ID is already active",
                    transactionId));
            }
        }
    } catch(const std::exception& exception) {
        return foundation::Result<std::unique_ptr<ApplicationTransaction>>::failure(
            foundation::makeError(
                "Transaction.BeginFailed",
                foundation::ErrorCategory::Internal,
                "The application transaction could not reserve its stable ID",
                foundation::Value {foundation::Value::Object {
                    {"transactionId", foundation::Value {std::string(transactionId.value())}},
                    {"reason", foundation::Value {exception.what()}},
                }}));
    } catch(...) {
        return foundation::Result<std::unique_ptr<ApplicationTransaction>>::failure(managerError(
            "Transaction.BeginFailed",
            foundation::ErrorCategory::Internal,
            "The application transaction could not reserve its stable ID",
            transactionId));
    }

    try {
        auto document = documents_.snapshot(documentId);
        if(!document.hasValue()) {
            release(transactionId);
            return foundation::Result<std::unique_ptr<ApplicationTransaction>>::failure(
                std::move(document).error());
        }
        auto validated = state::RevisionManager::validate(
            document.value().revisions(), preconditions);
        if(!validated.hasValue()) {
            release(transactionId);
            return foundation::Result<std::unique_ptr<ApplicationTransaction>>::failure(
                std::move(validated).error());
        }

        return foundation::Result<std::unique_ptr<ApplicationTransaction>>::success(
            std::unique_ptr<ApplicationTransaction>(new ApplicationTransaction(
                *this, transactionId, std::move(document).value())));
    } catch(const std::exception& exception) {
        release(transactionId);
        return foundation::Result<std::unique_ptr<ApplicationTransaction>>::failure(
            foundation::makeError(
                "Transaction.BeginFailed",
                foundation::ErrorCategory::Internal,
                "The application transaction could not begin",
                foundation::Value {foundation::Value::Object {
                    {"transactionId", foundation::Value {std::string(transactionId.value())}},
                    {"reason", foundation::Value {exception.what()}},
                }}));
    } catch(...) {
        release(transactionId);
        return foundation::Result<std::unique_ptr<ApplicationTransaction>>::failure(managerError(
            "Transaction.BeginFailed",
            foundation::ErrorCategory::Internal,
            "The application transaction could not begin",
            transactionId));
    }
}

std::size_t TransactionManager::activeTransactionCount() const
{
    std::lock_guard lock(activeMutex_);
    return activeTransactions_.size();
}

std::size_t TransactionManager::activeTransactionCount(
    const kernel::DocumentId& documentId) const
{
    std::lock_guard lock(activeMutex_);
    std::size_t count = 0U;
    for(const auto& [unusedTransactionId, activeDocumentId] : activeTransactions_) {
        static_cast<void>(unusedTransactionId);
        if(activeDocumentId == documentId) {
            ++count;
        }
    }
    return count;
}

foundation::Result<TransactionCommit> TransactionManager::commit(
    ApplicationTransaction& transaction,
    std::vector<ObjectChange> changes)
{
    // Validate the complete candidate before locks, journal writes or history changes.
    // 中文翻译：完整候选状态在加锁、写日志和变更历史之前校验。
    if(objectTypes_ != nullptr) {
        auto admitted = objectTypes_->validateObjects(
            transaction.stagedObjects_.all(), persistence_ != nullptr && persistence_->configured());
        if(!admitted) {
            return foundation::Result<TransactionCommit>::failure(std::move(admitted).error());
        }
    }
    std::lock_guard commitLock(commitMutex_);
    std::optional<TransactionCommit> receipt;
    {
        std::unique_lock documentLock(documents_.mutex_);
        const auto documentIterator = documents_.documents_.find(transaction.documentId());
        if(documentIterator == documents_.documents_.end()) {
            return foundation::Result<TransactionCommit>::failure(managerError(
                "Document.NotFound",
                foundation::ErrorCategory::NotFound,
                "The transaction document is no longer available",
                transaction.id()));
        }
        const auto projectIterator = documents_.projectRevisions_.find(
            documentIterator->second.projectId);
        if(projectIterator == documents_.projectRevisions_.end()) {
            return foundation::Result<TransactionCommit>::failure(managerError(
                "Document.ProjectRevisionMissing",
                foundation::ErrorCategory::Internal,
                "The transaction project revision is missing",
                transaction.id()));
        }

        auto currentRevisions = documentIterator->second.revisions;
        currentRevisions.atMutable(state::RevisionScope::Project) = projectIterator->second;
        std::array<state::RevisionPrecondition, allRevisionScopes.size()> capturedRevisions;
        for(std::size_t index = 0; index < allRevisionScopes.size(); ++index) {
            capturedRevisions[index] = state::RevisionPrecondition {
                allRevisionScopes[index],
                transaction.baseRevisions().at(allRevisionScopes[index])};
        }
        auto valid = state::RevisionManager::validate(currentRevisions, capturedRevisions);
        if(!valid.hasValue()) {
            return foundation::Result<TransactionCommit>::failure(std::move(valid).error());
        }

        std::array<state::RevisionScope, ApplicationTransaction::revisionScopeCount>
            affectedScopes;
        std::size_t affectedScopeCount = 0U;
        for(std::size_t index = 0; index < allRevisionScopes.size(); ++index) {
            if(transaction.affectedScopes_[index]) {
                affectedScopes[affectedScopeCount] = allRevisionScopes[index];
                ++affectedScopeCount;
            }
        }
        auto nextRevisions = state::RevisionManager::advance(
            currentRevisions,
            std::span<const state::RevisionScope> {
                affectedScopes.data(), affectedScopeCount});
        if(!nextRevisions.hasValue()) {
            return foundation::Result<TransactionCommit>::failure(
                std::move(nextRevisions).error());
        }

        std::vector<messaging::CommittedDomainEvent> committedEvents;
        committedEvents.reserve(transaction.pendingEvents_.size());
        for(std::size_t index = 0; index < transaction.pendingEvents_.size(); ++index) {
            const auto& pending = transaction.pendingEvents_[index];
            committedEvents.push_back(messaging::CommittedDomainEvent(
                pending.name,
                pending.version,
                pending.aggregateId,
                pending.payload,
                transaction.id(),
                transaction.projectId(),
                transaction.documentId(),
                nextRevisions.value(),
                index));
        }

        receipt.emplace(TransactionCommit {
            transaction.id(),
            transaction.projectId(),
            transaction.documentId(),
            currentRevisions,
            nextRevisions.value(),
            std::move(changes),
            std::move(committedEvents),
            transaction.historyMutation_});
    }

    std::optional<HistoryRuntime::DocumentHistory> preparedHistory;
    if(historyRuntime_ != nullptr) {
        auto prepared = historyRuntime_->prepareCommit(*receipt);
        if(!prepared) {
            return foundation::Result<TransactionCommit>::failure(
                std::move(prepared).error());
        }
        preparedHistory.emplace(std::move(prepared).value());
    }

    if(persistence_ != nullptr && persistence_->configured()) {
        auto persisted = persistence_->append(*receipt, transaction.idempotency_);
        if(!persisted) {
            return foundation::Result<TransactionCommit>::failure(
                std::move(persisted).error());
        }
    }

    {
        std::unique_lock documentLock(documents_.mutex_);
        const auto documentIterator = documents_.documents_.find(transaction.documentId());
        const auto projectIterator = documentIterator == documents_.documents_.end()
            ? documents_.projectRevisions_.end()
            : documents_.projectRevisions_.find(documentIterator->second.projectId);
        if(documentIterator == documents_.documents_.end()
           || projectIterator == documents_.projectRevisions_.end()) {
            return foundation::Result<TransactionCommit>::failure(managerError(
                "Document.StateChangedDuringPersistence",
                foundation::ErrorCategory::Internal,
                "Document ownership changed while its journal record was being persisted",
                transaction.id()));
        }
        documentIterator->second.objects.swap(transaction.stagedObjects_);
        documentIterator->second.revisions = receipt->revisionsAfter;
        projectIterator->second = receipt->revisionsAfter.at(state::RevisionScope::Project);
    }
    if(preparedHistory.has_value()) {
        historyRuntime_->install(std::move(*preparedHistory));
    }
    return foundation::Result<TransactionCommit>::success(std::move(*receipt));
}

void TransactionManager::release(const kernel::TransactionId& transactionId) noexcept
{
    std::lock_guard lock(activeMutex_);
    activeTransactions_.erase(transactionId);
}

} // namespace lasercnc::runtime
