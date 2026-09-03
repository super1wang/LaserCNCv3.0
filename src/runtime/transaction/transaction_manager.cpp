#include <lasercnc/runtime/transaction_manager.hpp>

#include <lasercnc/foundation/error.hpp>

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

TransactionManager::TransactionManager(state::DocumentStore& documents) noexcept
    : documents_(documents)
{
}

foundation::Result<std::unique_ptr<ApplicationTransaction>> TransactionManager::begin(
    kernel::TransactionId transactionId,
    const kernel::DocumentId& documentId,
    std::span<const state::RevisionPrecondition> preconditions)
{
    try {
        {
            std::lock_guard lock(activeMutex_);
            const auto [unused, inserted] = activeTransactions_.insert(transactionId);
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

foundation::Result<TransactionCommit> TransactionManager::commit(
    ApplicationTransaction& transaction,
    std::vector<ObjectChange> changes)
{
    std::unique_lock lock(documents_.mutex_);
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

    std::array<state::RevisionScope, ApplicationTransaction::revisionScopeCount> affectedScopes;
    std::size_t affectedScopeCount = 0U;
    for(std::size_t index = 0; index < allRevisionScopes.size(); ++index) {
        if(transaction.affectedScopes_[index]) {
            affectedScopes[affectedScopeCount] = allRevisionScopes[index];
            ++affectedScopeCount;
        }
    }
    auto nextRevisions = state::RevisionManager::advance(
        currentRevisions,
        std::span<const state::RevisionScope> {affectedScopes.data(), affectedScopeCount});
    if(!nextRevisions.hasValue()) {
        return foundation::Result<TransactionCommit>::failure(std::move(nextRevisions).error());
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

    TransactionCommit receipt {
        transaction.id(),
        transaction.projectId(),
        transaction.documentId(),
        currentRevisions,
        nextRevisions.value(),
        std::move(changes),
        std::move(committedEvents)};

    documentIterator->second.objects.swap(transaction.stagedObjects_);
    documentIterator->second.revisions = receipt.revisionsAfter;
    projectIterator->second = receipt.revisionsAfter.at(state::RevisionScope::Project);
    return foundation::Result<TransactionCommit>::success(std::move(receipt));
}

void TransactionManager::release(const kernel::TransactionId& transactionId) noexcept
{
    std::lock_guard lock(activeMutex_);
    activeTransactions_.erase(transactionId);
}

} // namespace lasercnc::runtime
