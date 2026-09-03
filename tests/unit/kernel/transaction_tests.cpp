#include <lasercnc/runtime/transaction_manager.hpp>
#include <lasercnc/kernel/app_kernel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

using namespace lasercnc::foundation;
using namespace lasercnc::kernel;
using namespace lasercnc::messaging;
using namespace lasercnc::runtime;
using namespace lasercnc::state;

namespace {

template <typename Id>
Id validId(const char* value)
{
    auto created = Id::create(value);
    if(!created.hasValue()) {
        throw std::logic_error("Invalid test ID");
    }
    return std::move(created).value();
}

struct TransactionFixture final {
    TransactionFixture()
        : project(validId<ProjectId>("project.test")),
          document(validId<DocumentId>("document.test")),
          manager(store)
    {
        auto added = store.addDocument(project, document);
        if(!added.hasValue()) {
            throw std::logic_error("Cannot add test document");
        }
    }

    [[nodiscard]] std::unique_ptr<ApplicationTransaction> begin(const char* transactionId)
    {
        auto begun = manager.begin(validId<TransactionId>(transactionId), document);
        if(!begun.hasValue()) {
            throw std::logic_error("Cannot begin test transaction");
        }
        return std::move(begun).value();
    }

    DocumentStore store;
    ProjectId project;
    DocumentId document;
    TransactionManager manager;
};

ObjectRecord objectRecord(const char* id, const char* value)
{
    return ObjectRecord {
        validId<ObjectId>(id),
        validId<ObjectTypeId>("kernel.test.object"),
        Value {value}};
}

PendingDomainEvent pendingEvent(const char* name, const ObjectId& aggregate, const char* value)
{
    return PendingDomainEvent {
        validId<EventName>(name),
        Version {1U, 0U, 0U},
        aggregate,
        Value {Value::Object {{"value", Value {value}}}}};
}

} // namespace

TEST_CASE("ApplicationTransaction commits objects revisions changes and events atomically", "[runtime][transaction]")
{
    STATIC_REQUIRE_FALSE(std::is_aggregate_v<CommittedDomainEvent>);
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<CommittedDomainEvent>);
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<ApplicationTransaction>);

    TransactionFixture fixture;
    auto transaction = fixture.begin("transaction.commit");
    const auto objectId = validId<ObjectId>("object.alpha");

    REQUIRE(transaction->createObject(objectRecord("object.alpha", "before")).hasValue());
    REQUIRE(transaction->touchRevision(RevisionScope::Geometry).hasValue());
    REQUIRE(transaction->collectEvent(
        pendingEvent("document.object-created", objectId, "before"))
                .hasValue());
    REQUIRE(transaction->collectEvent(
        pendingEvent("document.object-indexed", objectId, "before"))
                .hasValue());

    auto beforeCommit = fixture.store.snapshot(fixture.document);
    REQUIRE(beforeCommit.hasValue());
    CHECK(beforeCommit.value().objects().empty());
    CHECK(fixture.manager.activeTransactionCount() == 1U);

    auto committed = transaction->commit();
    REQUIRE(committed.hasValue());
    CHECK(transaction->transactionState() == TransactionState::Committed);
    CHECK(fixture.manager.activeTransactionCount() == 0U);
    CHECK(committed.value().transactionId == validId<TransactionId>("transaction.commit"));
    CHECK(committed.value().projectId == fixture.project);
    CHECK(committed.value().documentId == fixture.document);
    CHECK(committed.value().revisionsBefore.at(RevisionScope::Document) == Revision {0U});
    CHECK(committed.value().revisionsAfter.at(RevisionScope::Project) == Revision {1U});
    CHECK(committed.value().revisionsAfter.at(RevisionScope::Document) == Revision {1U});
    CHECK(committed.value().revisionsAfter.at(RevisionScope::Geometry) == Revision {1U});
    CHECK(committed.value().revisionsAfter.at(RevisionScope::Cam) == Revision {0U});

    REQUIRE(committed.value().changes.size() == 1U);
    CHECK(committed.value().changes.front().kind == ObjectChangeKind::Created);
    CHECK_FALSE(committed.value().changes.front().before.has_value());
    REQUIRE(committed.value().changes.front().after.has_value());
    CHECK(committed.value().changes.front().after->id == objectId);

    REQUIRE(committed.value().events.size() == 2U);
    const auto& event = committed.value().events.front();
    CHECK(event.name() == validId<EventName>("document.object-created"));
    CHECK(event.version() == Version {1U, 0U, 0U});
    REQUIRE(event.aggregateId().has_value());
    CHECK(*event.aggregateId() == objectId);
    CHECK(event.transactionId() == validId<TransactionId>("transaction.commit"));
    CHECK(event.projectId() == fixture.project);
    CHECK(event.documentId() == fixture.document);
    CHECK(event.revisions() == committed.value().revisionsAfter);
    CHECK(event.sequence() == 0U);
    CHECK(committed.value().events[1].name() == validId<EventName>("document.object-indexed"));
    CHECK(committed.value().events[1].sequence() == 1U);

    auto afterCommit = fixture.store.snapshot(fixture.document);
    REQUIRE(afterCommit.hasValue());
    const auto* object = afterCommit.value().objects().find(objectId);
    REQUIRE(object != nullptr);
    CHECK(*object->data.getIf<std::string>() == "before");

    auto committedAgain = transaction->commit();
    REQUIRE_FALSE(committedAgain.hasValue());
    CHECK(std::string(committedAgain.error().code.value()) == "Transaction.NotActive");
    auto rollbackCommitted = transaction->rollback();
    REQUIRE_FALSE(rollbackCommitted.hasValue());
    CHECK(std::string(rollbackCommitted.error().code.value()) == "Transaction.AlreadyCommitted");
}

TEST_CASE("ApplicationTransaction rollback and abandonment leave no state", "[runtime][transaction]")
{
    TransactionFixture fixture;
    {
        auto transaction = fixture.begin("transaction.rollback");
        REQUIRE(transaction->createObject(objectRecord("object.rollback", "discarded")).hasValue());
        REQUIRE(transaction->rollback().hasValue());
        CHECK(transaction->transactionState() == TransactionState::RolledBack);
        CHECK(transaction->rollback().hasValue());
    }
    {
        auto transaction = fixture.begin("transaction.abandoned");
        REQUIRE(transaction->createObject(objectRecord("object.abandoned", "discarded")).hasValue());
        CHECK(fixture.manager.activeTransactionCount() == 1U);
    }

    CHECK(fixture.manager.activeTransactionCount() == 0U);
    auto snapshot = fixture.store.snapshot(fixture.document);
    REQUIRE(snapshot.hasValue());
    CHECK(snapshot.value().objects().empty());
    CHECK(snapshot.value().revisions().at(RevisionScope::Document) == Revision {0U});
}

TEST_CASE("ApplicationTransaction is poisoned by any mutation failure", "[runtime][transaction]")
{
    TransactionFixture fixture;
    auto transaction = fixture.begin("transaction.poisoned");
    REQUIRE(transaction->createObject(objectRecord("object.same", "first")).hasValue());

    auto duplicate = transaction->createObject(objectRecord("object.same", "second"));
    REQUIRE_FALSE(duplicate.hasValue());
    CHECK(std::string(duplicate.error().code.value()) == "Document.ObjectAlreadyExists");
    CHECK(transaction->transactionState() == TransactionState::Failed);

    auto furtherMutation = transaction->removeObject(validId<ObjectId>("object.same"));
    REQUIRE_FALSE(furtherMutation.hasValue());
    CHECK(std::string(furtherMutation.error().code.value()) == "Transaction.Failed");

    auto committed = transaction->commit();
    REQUIRE_FALSE(committed.hasValue());
    CHECK(std::string(committed.error().code.value()) == "Transaction.Failed");
    REQUIRE(committed.error().cause != nullptr);
    CHECK(std::string(committed.error().cause->code.value()) == "Document.ObjectAlreadyExists");
    CHECK(transaction->transactionState() == TransactionState::RolledBack);

    auto snapshot = fixture.store.snapshot(fixture.document);
    REQUIRE(snapshot.hasValue());
    CHECK(snapshot.value().objects().empty());
    CHECK(snapshot.value().revisions().at(RevisionScope::Document) == Revision {0U});
}

TEST_CASE("ApplicationTransaction detects optimistic revision conflicts", "[runtime][transaction]")
{
    TransactionFixture fixture;
    auto first = fixture.begin("transaction.first");
    auto second = fixture.begin("transaction.second");
    REQUIRE(first->createObject(objectRecord("object.first", "first")).hasValue());
    REQUIRE(second->createObject(objectRecord("object.second", "second")).hasValue());
    REQUIRE(second->collectEvent(pendingEvent(
        "document.object-created", validId<ObjectId>("object.second"), "second"))
                .hasValue());

    REQUIRE(first->commit().hasValue());
    auto stale = second->commit();
    REQUIRE_FALSE(stale.hasValue());
    CHECK(std::string(stale.error().code.value()) == "Project.RevisionConflict");
    CHECK(second->transactionState() == TransactionState::RolledBack);

    auto snapshot = fixture.store.snapshot(fixture.document);
    REQUIRE(snapshot.hasValue());
    CHECK(snapshot.value().objects().contains(validId<ObjectId>("object.first")));
    CHECK_FALSE(snapshot.value().objects().contains(validId<ObjectId>("object.second")));
    CHECK(snapshot.value().revisions().at(RevisionScope::Document) == Revision {1U});
}

TEST_CASE("TransactionManager validates begin preconditions and active IDs", "[runtime][transaction]")
{
    TransactionFixture fixture;
    auto active = fixture.begin("transaction.duplicate");
    auto duplicate = fixture.manager.begin(
        validId<TransactionId>("transaction.duplicate"), fixture.document);
    REQUIRE_FALSE(duplicate.hasValue());
    CHECK(std::string(duplicate.error().code.value()) == "Transaction.AlreadyActive");
    REQUIRE(active->rollback().hasValue());

    const std::array mismatch {
        RevisionPrecondition {RevisionScope::Document, Revision {1U}},
    };
    auto stale = fixture.manager.begin(
        validId<TransactionId>("transaction.stale"), fixture.document, mismatch);
    REQUIRE_FALSE(stale.hasValue());
    CHECK(std::string(stale.error().code.value()) == "Project.RevisionConflict");
    CHECK(fixture.manager.activeTransactionCount() == 0U);

    auto missing = fixture.manager.begin(
        validId<TransactionId>("transaction.missing"),
        validId<DocumentId>("document.missing"));
    REQUIRE_FALSE(missing.hasValue());
    CHECK(std::string(missing.error().code.value()) == "Document.NotFound");
    CHECK(fixture.manager.activeTransactionCount() == 0U);
}

TEST_CASE("Project revision conflicts stale transactions across documents", "[runtime][transaction]")
{
    TransactionFixture fixture;
    const auto secondDocument = validId<DocumentId>("document.other");
    REQUIRE(fixture.store.addDocument(fixture.project, secondDocument).hasValue());

    auto onOther = fixture.manager.begin(
        validId<TransactionId>("transaction.other-stale"), secondDocument);
    REQUIRE(onOther.hasValue());
    REQUIRE(onOther.value()->createObject(objectRecord("object.other", "other")).hasValue());

    auto onFirst = fixture.begin("transaction.first-project-change");
    REQUIRE(onFirst->createObject(objectRecord("object.first", "first")).hasValue());
    REQUIRE(onFirst->commit().hasValue());

    auto stale = onOther.value()->commit();
    REQUIRE_FALSE(stale.hasValue());
    CHECK(std::string(stale.error().code.value()) == "Project.RevisionConflict");

    auto otherSnapshot = fixture.store.snapshot(secondDocument);
    REQUIRE(otherSnapshot.hasValue());
    CHECK(otherSnapshot.value().revisions().at(RevisionScope::Project) == Revision {1U});
    CHECK(otherSnapshot.value().revisions().at(RevisionScope::Document) == Revision {0U});

    auto retry = fixture.manager.begin(
        validId<TransactionId>("transaction.other-retry"), secondDocument);
    REQUIRE(retry.hasValue());
    REQUIRE(retry.value()->createObject(objectRecord("object.other", "other")).hasValue());
    REQUIRE(retry.value()->touchRevision(RevisionScope::Cam).hasValue());
    auto committed = retry.value()->commit();
    REQUIRE(committed.hasValue());
    CHECK(committed.value().revisionsAfter.at(RevisionScope::Project) == Revision {2U});
    CHECK(committed.value().revisionsAfter.at(RevisionScope::Document) == Revision {1U});
    CHECK(committed.value().revisionsAfter.at(RevisionScope::Cam) == Revision {1U});

    auto firstSnapshot = fixture.store.snapshot(fixture.document);
    REQUIRE(firstSnapshot.hasValue());
    CHECK(firstSnapshot.value().revisions().at(RevisionScope::Project) == Revision {2U});
    CHECK(firstSnapshot.value().revisions().at(RevisionScope::Document) == Revision {1U});
}

TEST_CASE("Transaction changes preserve before and after data for future undo", "[runtime][transaction]")
{
    TransactionFixture fixture;
    const auto objectId = validId<ObjectId>("object.undo");
    auto seed = fixture.begin("transaction.seed");
    REQUIRE(seed->createObject(objectRecord("object.undo", "v1")).hasValue());
    REQUIRE(seed->commit().hasValue());

    auto update = fixture.begin("transaction.update");
    REQUIRE(update->replaceObjectData(objectId, Value {"v2"}).hasValue());
    auto updated = update->commit();
    REQUIRE(updated.hasValue());
    REQUIRE(updated.value().changes.size() == 1U);
    const auto& updateChange = updated.value().changes.front();
    CHECK(updateChange.kind == ObjectChangeKind::Updated);
    REQUIRE(updateChange.before.has_value());
    REQUIRE(updateChange.after.has_value());
    CHECK(*updateChange.before->data.getIf<std::string>() == "v1");
    CHECK(*updateChange.after->data.getIf<std::string>() == "v2");

    auto remove = fixture.begin("transaction.remove");
    REQUIRE(remove->removeObject(objectId).hasValue());
    auto removed = remove->commit();
    REQUIRE(removed.hasValue());
    REQUIRE(removed.value().changes.size() == 1U);
    CHECK(removed.value().changes.front().kind == ObjectChangeKind::Removed);
    REQUIRE(removed.value().changes.front().before.has_value());
    CHECK_FALSE(removed.value().changes.front().after.has_value());
}

TEST_CASE("Stable object IDs cannot be reincarnated inside one transaction", "[runtime][transaction]")
{
    TransactionFixture fixture;
    const auto objectId = validId<ObjectId>("object.stable");
    auto seed = fixture.begin("transaction.seed-stable");
    REQUIRE(seed->createObject(objectRecord("object.stable", "original")).hasValue());
    REQUIRE(seed->commit().hasValue());

    auto replacement = fixture.begin("transaction.reincarnation");
    REQUIRE(replacement->removeObject(objectId).hasValue());
    auto reused = replacement->createObject(objectRecord("object.stable", "replacement"));
    REQUIRE_FALSE(reused.hasValue());
    CHECK(std::string(reused.error().code.value()) == "Document.ObjectIdReuseDenied");
    REQUIRE_FALSE(replacement->commit().hasValue());

    auto snapshot = fixture.store.snapshot(fixture.document);
    REQUIRE(snapshot.hasValue());
    const auto* object = snapshot.value().objects().find(objectId);
    REQUIRE(object != nullptr);
    CHECK(*object->data.getIf<std::string>() == "original");
}

TEST_CASE("Empty net changes cannot advance revisions or release events", "[runtime][transaction]")
{
    TransactionFixture fixture;
    auto transaction = fixture.begin("transaction.empty");
    const auto objectId = validId<ObjectId>("object.temporary");
    REQUIRE(transaction->createObject(objectRecord("object.temporary", "temporary")).hasValue());
    REQUIRE(transaction->removeObject(objectId).hasValue());
    REQUIRE(transaction->collectEvent(
        pendingEvent("document.temporary", objectId, "temporary"))
                .hasValue());

    auto empty = transaction->commit();
    REQUIRE_FALSE(empty.hasValue());
    CHECK(std::string(empty.error().code.value()) == "Transaction.EmptyCommitDenied");
    CHECK(transaction->transactionState() == TransactionState::RolledBack);
    auto snapshot = fixture.store.snapshot(fixture.document);
    REQUIRE(snapshot.hasValue());
    CHECK(snapshot.value().objects().empty());
    CHECK(snapshot.value().revisions().at(RevisionScope::Project) == Revision {0U});
}

TEST_CASE("Concurrent transaction commits serialize with exactly one winner", "[runtime][transaction]")
{
    TransactionFixture fixture;
    auto first = fixture.begin("transaction.concurrent-first");
    auto second = fixture.begin("transaction.concurrent-second");
    REQUIRE(first->createObject(objectRecord("object.concurrent-first", "first")).hasValue());
    REQUIRE(second->createObject(objectRecord("object.concurrent-second", "second")).hasValue());

    auto firstResult = std::async(std::launch::async, [&first]() {
        return first->commit().hasValue();
    });
    auto secondResult = std::async(std::launch::async, [&second]() {
        return second->commit().hasValue();
    });
    const bool firstCommitted = firstResult.get();
    const bool secondCommitted = secondResult.get();
    CHECK(firstCommitted != secondCommitted);

    auto snapshot = fixture.store.snapshot(fixture.document);
    REQUIRE(snapshot.hasValue());
    CHECK(snapshot.value().objects().size() == 1U);
    CHECK(snapshot.value().revisions().at(RevisionScope::Project) == Revision {1U});
    CHECK(snapshot.value().revisions().at(RevisionScope::Document) == Revision {1U});
    CHECK(fixture.manager.activeTransactionCount() == 0U);
}

TEST_CASE("AppKernel exposes document loading only during composition", "[kernel][state]")
{
    lasercnc::kernel::AppKernel kernel;
    const auto project = validId<ProjectId>("project.kernel");
    const auto document = validId<DocumentId>("document.kernel");
    REQUIRE(kernel.addDocument(project, document).hasValue());
    REQUIRE(kernel.bootstrap().hasValue());
    CHECK(kernel.documents().contains(document));
    auto late = kernel.addDocument(project, validId<DocumentId>("document.late"));
    REQUIRE_FALSE(late.hasValue());
    CHECK(std::string(late.error().code.value()) == "Kernel.DocumentLoadNotConfiguring");
}
