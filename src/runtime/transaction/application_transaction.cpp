#include <lasercnc/runtime/transaction.hpp>

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/runtime/transaction_manager.hpp>

#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace lasercnc::runtime {
namespace {

foundation::Error transactionError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const kernel::TransactionId& transactionId,
    std::shared_ptr<const foundation::Error> cause = nullptr)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {foundation::Value::Object {
            {"transactionId", foundation::Value {std::string(transactionId.value())}},
        }},
        foundation::Severity::Error,
        std::move(cause));
}

foundation::Error unexpectedFailure(
    const kernel::TransactionId& transactionId,
    const char* operation,
    const char* reason)
{
    return foundation::makeError(
        "Transaction.InternalFailure",
        foundation::ErrorCategory::Internal,
        "Application transaction operation failed unexpectedly",
        foundation::Value {foundation::Value::Object {
            {"transactionId", foundation::Value {std::string(transactionId.value())}},
            {"operation", foundation::Value {operation}},
            {"reason", foundation::Value {reason}},
        }});
}

std::size_t revisionScopeIndex(state::RevisionScope scope)
{
    switch(scope) {
    case state::RevisionScope::Project: return 0U;
    case state::RevisionScope::Document: return 1U;
    case state::RevisionScope::Geometry: return 2U;
    case state::RevisionScope::Cam: return 3U;
    case state::RevisionScope::MachineContext: return 4U;
    case state::RevisionScope::Environment: return 5U;
    }
    return 6U;
}

} // namespace

ApplicationTransaction::ApplicationTransaction(
    TransactionManager& manager,
    kernel::TransactionId transactionId,
    state::Document baseDocument)
    : manager_(&manager),
      transactionId_(std::move(transactionId)),
      baseDocument_(std::move(baseDocument)),
      stagedObjects_(baseDocument_.objects())
{
}

ApplicationTransaction::~ApplicationTransaction()
{
    if(manager_ != nullptr) {
        manager_->release(transactionId_);
        manager_ = nullptr;
        state_ = TransactionState::RolledBack;
    }
}

const kernel::TransactionId& ApplicationTransaction::id() const noexcept
{
    return transactionId_;
}

const kernel::ProjectId& ApplicationTransaction::projectId() const noexcept
{
    return baseDocument_.projectId();
}

const kernel::DocumentId& ApplicationTransaction::documentId() const noexcept
{
    return baseDocument_.id();
}

const state::RevisionSet& ApplicationTransaction::baseRevisions() const noexcept
{
    return baseDocument_.revisions();
}

TransactionState ApplicationTransaction::transactionState() const noexcept
{
    return state_;
}

const state::ObjectRegistry& ApplicationTransaction::stagedObjects() const noexcept
{
    return stagedObjects_;
}

foundation::Result<void> ApplicationTransaction::ensureActive() const
{
    if(state_ == TransactionState::Active) {
        return foundation::Result<void>::success();
    }
    if(state_ == TransactionState::Failed) {
        std::shared_ptr<const foundation::Error> cause;
        if(failure_.has_value()) {
            cause = std::make_shared<const foundation::Error>(*failure_);
        }
        return foundation::Result<void>::failure(transactionError(
            "Transaction.Failed",
            foundation::ErrorCategory::Conflict,
            "The transaction is poisoned by an earlier mutation failure",
            transactionId_,
            std::move(cause)));
    }
    return foundation::Result<void>::failure(transactionError(
        "Transaction.NotActive",
        foundation::ErrorCategory::Conflict,
        "The transaction is no longer active",
        transactionId_));
}

foundation::Result<void> ApplicationTransaction::fail(foundation::Error error)
{
    failure_ = error;
    state_ = TransactionState::Failed;
    return foundation::Result<void>::failure(std::move(error));
}

void ApplicationTransaction::markDocumentChanged() noexcept
{
    affectedScopes_[revisionScopeIndex(state::RevisionScope::Project)] = true;
    affectedScopes_[revisionScopeIndex(state::RevisionScope::Document)] = true;
}

foundation::Result<void> ApplicationTransaction::createObject(state::ObjectRecord object)
{
    auto active = ensureActive();
    if(!active.hasValue()) {
        return active;
    }
    try {
        if(baseDocument_.objects().contains(object.id) && !stagedObjects_.contains(object.id)) {
            return fail(foundation::makeError(
                "Document.ObjectIdReuseDenied",
                foundation::ErrorCategory::Conflict,
                "A removed stable object ID cannot be reused in the same transaction",
                foundation::Value {foundation::Value::Object {
                    {"objectId", foundation::Value {std::string(object.id.value())}},
                }}));
        }
        auto inserted = stagedObjects_.insert(std::move(object));
        if(!inserted.hasValue()) {
            return fail(std::move(inserted).error());
        }
        markDocumentChanged();
        return foundation::Result<void>::success();
    } catch(const std::exception& exception) {
        return fail(unexpectedFailure(transactionId_, "createObject", exception.what()));
    } catch(...) {
        return fail(unexpectedFailure(transactionId_, "createObject", "Unknown failure"));
    }
}

foundation::Result<void> ApplicationTransaction::replaceObjectData(
    const kernel::ObjectId& objectId,
    foundation::Value data)
{
    auto active = ensureActive();
    if(!active.hasValue()) {
        return active;
    }
    try {
        auto replaced = stagedObjects_.replaceData(objectId, std::move(data));
        if(!replaced.hasValue()) {
            return fail(std::move(replaced).error());
        }
        markDocumentChanged();
        return foundation::Result<void>::success();
    } catch(const std::exception& exception) {
        return fail(unexpectedFailure(transactionId_, "replaceObjectData", exception.what()));
    } catch(...) {
        return fail(unexpectedFailure(transactionId_, "replaceObjectData", "Unknown failure"));
    }
}

foundation::Result<void> ApplicationTransaction::removeObject(
    const kernel::ObjectId& objectId)
{
    auto active = ensureActive();
    if(!active.hasValue()) {
        return active;
    }
    try {
        auto removed = stagedObjects_.erase(objectId);
        if(!removed.hasValue()) {
            return fail(std::move(removed).error());
        }
        markDocumentChanged();
        return foundation::Result<void>::success();
    } catch(const std::exception& exception) {
        return fail(unexpectedFailure(transactionId_, "removeObject", exception.what()));
    } catch(...) {
        return fail(unexpectedFailure(transactionId_, "removeObject", "Unknown failure"));
    }
}

foundation::Result<void> ApplicationTransaction::touchRevision(state::RevisionScope scope)
{
    auto active = ensureActive();
    if(!active.hasValue()) {
        return active;
    }
    const auto index = revisionScopeIndex(scope);
    if(index >= revisionScopeCount) {
        return fail(foundation::makeError(
            "Revision.InvalidScope",
            foundation::ErrorCategory::Validation,
            "Revision scope is invalid"));
    }
    affectedScopes_[index] = true;
    return foundation::Result<void>::success();
}

foundation::Result<void> ApplicationTransaction::collectEvent(
    messaging::PendingDomainEvent event)
{
    auto active = ensureActive();
    if(!active.hasValue()) {
        return active;
    }
    try {
        pendingEvents_.push_back(std::move(event));
        return foundation::Result<void>::success();
    } catch(const std::exception& exception) {
        return fail(unexpectedFailure(transactionId_, "collectEvent", exception.what()));
    } catch(...) {
        return fail(unexpectedFailure(transactionId_, "collectEvent", "Unknown failure"));
    }
}

std::vector<ObjectChange> ApplicationTransaction::buildChanges() const
{
    const auto& before = baseDocument_.objects().objects_;
    const auto& after = stagedObjects_.objects_;
    auto beforeIterator = before.begin();
    auto afterIterator = after.begin();
    std::vector<ObjectChange> changes;
    changes.reserve(before.size() + after.size());

    while(beforeIterator != before.end() || afterIterator != after.end()) {
        if(beforeIterator == before.end()
           || (afterIterator != after.end() && afterIterator->first < beforeIterator->first)) {
            changes.push_back(ObjectChange {
                ObjectChangeKind::Created,
                afterIterator->first,
                std::nullopt,
                afterIterator->second});
            ++afterIterator;
            continue;
        }
        if(afterIterator == after.end() || beforeIterator->first < afterIterator->first) {
            changes.push_back(ObjectChange {
                ObjectChangeKind::Removed,
                beforeIterator->first,
                beforeIterator->second,
                std::nullopt});
            ++beforeIterator;
            continue;
        }
        if(beforeIterator->second != afterIterator->second) {
            changes.push_back(ObjectChange {
                ObjectChangeKind::Updated,
                beforeIterator->first,
                beforeIterator->second,
                afterIterator->second});
        }
        ++beforeIterator;
        ++afterIterator;
    }
    return changes;
}

foundation::Result<TransactionCommit> ApplicationTransaction::commit()
{
    auto active = ensureActive();
    if(!active.hasValue()) {
        if(state_ == TransactionState::Failed) {
            release(TransactionState::RolledBack);
        }
        return foundation::Result<TransactionCommit>::failure(std::move(active).error());
    }

    try {
        auto changes = buildChanges();
        if(changes.empty()) {
            auto error = transactionError(
                "Transaction.EmptyCommitDenied",
                foundation::ErrorCategory::Validation,
                "A transaction without a net document change cannot be committed",
                transactionId_);
            release(TransactionState::RolledBack);
            return foundation::Result<TransactionCommit>::failure(std::move(error));
        }

        auto committed = manager_->commit(*this, std::move(changes));
        release(committed.hasValue() ? TransactionState::Committed : TransactionState::RolledBack);
        return committed;
    } catch(const std::exception& exception) {
        auto error = unexpectedFailure(transactionId_, "commit", exception.what());
        release(TransactionState::RolledBack);
        return foundation::Result<TransactionCommit>::failure(std::move(error));
    } catch(...) {
        auto error = unexpectedFailure(transactionId_, "commit", "Unknown failure");
        release(TransactionState::RolledBack);
        return foundation::Result<TransactionCommit>::failure(std::move(error));
    }
}

foundation::Result<void> ApplicationTransaction::rollback()
{
    if(state_ == TransactionState::RolledBack) {
        return foundation::Result<void>::success();
    }
    if(state_ == TransactionState::Committed) {
        return foundation::Result<void>::failure(transactionError(
            "Transaction.AlreadyCommitted",
            foundation::ErrorCategory::Conflict,
            "A committed transaction cannot be rolled back in memory",
            transactionId_));
    }
    release(TransactionState::RolledBack);
    return foundation::Result<void>::success();
}

void ApplicationTransaction::release(TransactionState terminalState) noexcept
{
    state_ = terminalState;
    if(manager_ != nullptr) {
        manager_->release(transactionId_);
        manager_ = nullptr;
    }
}

} // namespace lasercnc::runtime
