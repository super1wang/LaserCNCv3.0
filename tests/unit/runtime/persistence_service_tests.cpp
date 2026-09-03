#include <lasercnc/infrastructure/jsoncons_adapter.hpp>
#include <lasercnc/infrastructure/sha256_hash_service.hpp>
#include <lasercnc/infrastructure/filesystem_snapshot_store.hpp>
#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>
#include <lasercnc/kernel/app_kernel.hpp>
#include <lasercnc/persistence/persistence_service.hpp>
#include <lasercnc/runtime/transaction_manager.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace lasercnc::foundation;
using namespace lasercnc::infrastructure;
using namespace lasercnc::kernel;
using namespace lasercnc::persistence;
using namespace lasercnc::runtime;
using namespace lasercnc::state;

namespace {

template <typename Id>
Id validId(const char* value)
{
    auto created = Id::create(value);
    if(!created) {
        throw std::logic_error("Invalid test ID");
    }
    return std::move(created).value();
}

std::filesystem::path uniqueDatabasePath()
{
    static std::atomic_ullong sequence {0U};
    return std::filesystem::temp_directory_path()
        / ("lasercnc-persistence-service-"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
           + '-' + std::to_string(sequence.fetch_add(1U)) + ".db");
}

std::filesystem::path uniqueSnapshotDirectory()
{
    static std::atomic_ullong sequence {0U};
    return std::filesystem::temp_directory_path()
        / ("lasercnc-persistence-snapshots-"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
           + '-' + std::to_string(sequence.fetch_add(1U)));
}

TransactionCommit commit(
    const char* transaction,
    RevisionSet before,
    RevisionSet after,
    const char* data)
{
    const auto objectId = validId<ObjectId>("object.persisted");
    return TransactionCommit {
        validId<TransactionId>(transaction),
        validId<ProjectId>("project.persisted"),
        validId<DocumentId>("document.persisted"),
        std::move(before),
        std::move(after),
        std::vector<ObjectChange> {ObjectChange {
            ObjectChangeKind::Created,
            objectId,
            std::nullopt,
            ObjectRecord {
                objectId,
                validId<ObjectTypeId>("kernel.persistence.test"),
                Value {data}}}},
        {}};
}

void removeDatabase(const std::filesystem::path& path)
{
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove(path, ignored));
    static_cast<void>(std::filesystem::remove(path.string() + "-wal", ignored));
    static_cast<void>(std::filesystem::remove(path.string() + "-shm", ignored));
}

void removeSnapshotDirectory(const std::filesystem::path& path)
{
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove_all(path, ignored));
}

void configureService(PersistenceService& service, const std::filesystem::path& path)
{
    auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
    REQUIRE(backend.hasValue());
    REQUIRE(service
                .configure(
                    std::move(backend).value(),
                    std::make_shared<JsonconsAdapter>(),
                    std::make_shared<Sha256HashService>())
                .hasValue());
    REQUIRE(service.initialize().hasValue());
}

void configureService(
    PersistenceService& service,
    const std::filesystem::path& path,
    const std::filesystem::path& snapshotDirectory)
{
    auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
    REQUIRE(backend.hasValue());
    auto snapshots = FilesystemSnapshotStore::create(
        FilesystemSnapshotStoreOptions {snapshotDirectory, 1024U * 1024U});
    REQUIRE(snapshots.hasValue());
    REQUIRE(service
                .configure(
                    std::move(backend).value(),
                    std::make_shared<JsonconsAdapter>(),
                    std::make_shared<Sha256HashService>(),
                    std::move(snapshots).value())
                .hasValue());
    REQUIRE(service.initialize().hasValue());
}

class ThrowingBackend final : public lasercnc::platform::IPersistenceBackend {
public:
    Result<std::size_t> execute(std::string_view, std::span<const Value>) override
    {
        throw std::runtime_error("expected backend exception");
    }

    Result<std::vector<lasercnc::platform::PersistenceRow>> query(
        std::string_view,
        std::span<const Value>) override
    {
        return Result<std::vector<lasercnc::platform::PersistenceRow>>::success({});
    }

    Result<void> beginTransaction() override
    {
        ++begins;
        active = true;
        return Result<void>::success();
    }

    Result<void> commitTransaction() override
    {
        active = false;
        return Result<void>::success();
    }

    Result<void> rollbackTransaction() override
    {
        ++rollbacks;
        active = false;
        return Result<void>::success();
    }

    std::size_t begins{0U};
    std::size_t rollbacks{0U};
    bool active{false};
};

class ReentrantSerializer final : public IValueSerializer {
public:
    ReentrantSerializer(DocumentStore& documents, DocumentId documentId)
        : documents_(documents), documentId_(std::move(documentId))
    {
    }

    Result<std::string> serialize(const Value& value) const override
    {
        auto snapshot = documents_.snapshot(documentId_);
        sawOldSnapshot = snapshot.hasValue()
            && snapshot.value().revisions().at(RevisionScope::Document) == Revision {0U};
        return json_.serialize(value);
    }

    Result<Value> deserialize(std::string_view payload) const override
    {
        return json_.deserialize(payload);
    }

    mutable bool sawOldSnapshot{false};

private:
    DocumentStore& documents_;
    DocumentId documentId_;
    JsonconsAdapter json_;
};

class FailingSerializer final : public IValueSerializer {
public:
    Result<std::string> serialize(const Value&) const override
    {
        return Result<std::string>::failure(makeError(
            "Test.SerializeFailed", ErrorCategory::Infrastructure, "expected"));
    }

    Result<Value> deserialize(std::string_view) const override
    {
        return Result<Value>::failure(makeError(
            "Test.DeserializeFailed", ErrorCategory::Infrastructure, "expected"));
    }
};

} // namespace

TEST_CASE("PersistenceService migrates and appends an idempotent state journal", "[persistence][journal]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    const RevisionSet zero;
    const RevisionSet one {
        Revision {1U}, Revision {1U}, Revision {1U}, Revision {}, Revision {}, Revision {}};
    const RevisionSet two {
        Revision {2U}, Revision {2U}, Revision {2U}, Revision {}, Revision {}, Revision {}};

    {
        PersistenceService service;
        CHECK_FALSE(service.initialize().hasValue());
        configureService(service, path);
        CHECK(service.configured());
        CHECK(service.ready());
        CHECK(service.initialize().hasValue());

        auto first = service.append(commit("transaction.persisted.1", zero, one, "one"));
        REQUIRE(first.hasValue());
        CHECK(first.value().sequence == 1U);
        CHECK(std::string(first.value().digest.value()).starts_with("sha256:"));

        auto replay = service.append(commit("transaction.persisted.1", zero, one, "one"));
        REQUIRE(replay.hasValue());
        CHECK(replay.value().sequence == first.value().sequence);
        CHECK(replay.value().digest == first.value().digest);

        auto conflict = service.append(commit("transaction.persisted.1", zero, one, "changed"));
        REQUIRE_FALSE(conflict.hasValue());
        CHECK(std::string(conflict.error().code.value())
              == "Persistence.JournalTransactionConflict");

        auto second = service.append(commit("transaction.persisted.2", one, two, "two"));
        REQUIRE(second.hasValue());
        CHECK(second.value().sequence == 2U);
        auto records = service.journalAfter(validId<DocumentId>("document.persisted"), 0U);
        REQUIRE(records.hasValue());
        REQUIRE(records.value().size() == 2U);
        CHECK(records.value()[0].sequence == 1U);
        CHECK(records.value()[1].sequence == 2U);
        CHECK(service.journalAfter(
            validId<DocumentId>("document.persisted"), 1U).value().size() == 1U);
    }

    {
        PersistenceService reopened;
        configureService(reopened, path);
        auto records = reopened.journalAfter(
            validId<DocumentId>("document.persisted"), 0U);
        REQUIRE(records.hasValue());
        CHECK(records.value().size() == 2U);
    }
    removeDatabase(path);
}

TEST_CASE("PersistenceService fails closed on journal corruption", "[persistence][journal][recovery]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    {
        PersistenceService service;
        configureService(service, path);
        const RevisionSet one {
            Revision {1U}, Revision {1U}, Revision {1U}, Revision {}, Revision {}, Revision {}};
        auto appended = service.append(
            commit("transaction.corrupt", RevisionSet {}, one, "safe"));
        REQUIRE(appended.hasValue());

        auto tamper = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(tamper.hasValue());
        const std::array parameters {
            Value {"tampered"}, Value {"transaction.corrupt"}};
        REQUIRE(tamper.value()
                    ->execute(
                        "UPDATE state_journal SET payload=? WHERE transaction_id=?",
                        parameters)
                    .hasValue());

        auto records = service.journalAfter(
            validId<DocumentId>("document.persisted"), 0U);
        REQUIRE_FALSE(records.hasValue());
        CHECK(std::string(records.error().code.value())
              == "Persistence.JournalDigestMismatch");

        const std::array metadataParameters {
            Value {appended.value().payload},
            Value {"project.tampered"},
            Value {"transaction.corrupt"}};
        REQUIRE(tamper.value()
                    ->execute(
                        "UPDATE state_journal SET payload=?,project_id=? "
                        "WHERE transaction_id=?",
                        metadataParameters)
                    .hasValue());
        auto metadataMismatch = service.journalAfter(
            validId<DocumentId>("document.persisted"), 0U);
        REQUIRE_FALSE(metadataMismatch.hasValue());
        CHECK(std::string(metadataMismatch.error().code.value())
              == "Persistence.JournalMetadataMismatch");
    }
    removeDatabase(path);
}

TEST_CASE("PersistenceService captures immutable snapshots aligned with the journal", "[persistence][snapshot]")
{
    const auto path = uniqueDatabasePath();
    const auto snapshotDirectory = uniqueSnapshotDirectory();
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
    const auto projectId = validId<ProjectId>("project.snapshot");
    const auto documentId = validId<DocumentId>("document.snapshot");
    const auto objectId = validId<ObjectId>("object.snapshot");
    const auto firstSnapshotId = validId<SnapshotId>("snapshot.capture-1");
    const auto secondSnapshotId = validId<SnapshotId>("snapshot.capture-2");

    {
        DocumentStore documents;
        REQUIRE(documents.addDocument(projectId, documentId).hasValue());
        PersistenceService persistence;
        configureService(persistence, path, snapshotDirectory);
        TransactionManager transactions(documents, &persistence);

        auto firstTransaction = transactions.begin(
            validId<TransactionId>("transaction.snapshot.1"), documentId);
        REQUIRE(firstTransaction.hasValue());
        REQUIRE(firstTransaction.value()
                    ->createObject(ObjectRecord {
                        objectId,
                        validId<ObjectTypeId>("kernel.persistence.snapshot"),
                        Value {"first"}})
                    .hasValue());
        REQUIRE(firstTransaction.value()->touchRevision(RevisionScope::Geometry).hasValue());
        REQUIRE(firstTransaction.value()->commit().hasValue());

        auto document = documents.snapshot(documentId);
        REQUIRE(document.hasValue());
        auto first = persistence.captureSnapshot(firstSnapshotId, document.value());
        REQUIRE(first.hasValue());
        CHECK(first.value().journalSequence == 1U);
        CHECK(first.value().revisions == document.value().revisions());
        CHECK(std::string(first.value().digest.value()).starts_with("sha256:"));
        CHECK(std::filesystem::is_regular_file(
            snapshotDirectory / "snapshot.capture-1.snapshot"));

        auto repeated = persistence.captureSnapshot(firstSnapshotId, document.value());
        REQUIRE(repeated.hasValue());
        CHECK(repeated.value().payload == first.value().payload);
        CHECK(repeated.value().digest == first.value().digest);
        auto latest = persistence.latestSnapshot(documentId);
        REQUIRE(latest.hasValue());
        REQUIRE(latest.value().has_value());
        CHECK(latest.value()->snapshotId == firstSnapshotId);

        auto secondTransaction = transactions.begin(
            validId<TransactionId>("transaction.snapshot.2"), documentId);
        REQUIRE(secondTransaction.hasValue());
        REQUIRE(secondTransaction.value()->replaceObjectData(objectId, Value {"second"}).hasValue());
        REQUIRE(secondTransaction.value()->commit().hasValue());
        document = documents.snapshot(documentId);
        REQUIRE(document.hasValue());

        auto identityConflict = persistence.captureSnapshot(
            firstSnapshotId, document.value());
        REQUIRE_FALSE(identityConflict.hasValue());
        CHECK(std::string(identityConflict.error().code.value())
              == "Persistence.SnapshotIdentityConflict");
        auto second = persistence.captureSnapshot(secondSnapshotId, document.value());
        REQUIRE(second.hasValue());
        CHECK(second.value().journalSequence == 2U);
    }

    {
        PersistenceService reopened;
        configureService(reopened, path, snapshotDirectory);
        auto latest = reopened.latestSnapshot(documentId);
        REQUIRE(latest.hasValue());
        REQUIRE(latest.value().has_value());
        CHECK(latest.value()->snapshotId == secondSnapshotId);
        CHECK(latest.value()->journalSequence == 2U);

        std::ofstream tampered(
            snapshotDirectory / "snapshot.capture-2.snapshot",
            std::ios::binary | std::ios::trunc);
        REQUIRE(tampered.good());
        tampered << "tampered";
        tampered.close();
        auto rejected = reopened.latestSnapshot(documentId);
        REQUIRE_FALSE(rejected.hasValue());
        CHECK((std::string(rejected.error().code.value())
                   == "Persistence.SnapshotSizeMismatch"
               || std::string(rejected.error().code.value())
                   == "Persistence.SnapshotDigestMismatch"));
        auto recovery = reopened.recover();
        REQUIRE_FALSE(recovery.hasValue());
        CHECK((std::string(recovery.error().code.value())
                   == "Persistence.SnapshotSizeMismatch"
               || std::string(recovery.error().code.value())
                   == "Persistence.SnapshotDigestMismatch"));
    }
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
}

TEST_CASE("PersistenceService rejects snapshots ahead of the journal", "[persistence][snapshot][recovery]")
{
    const auto path = uniqueDatabasePath();
    const auto snapshotDirectory = uniqueSnapshotDirectory();
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
    const auto projectId = validId<ProjectId>("project.snapshot-boundary");
    const auto emptyDocumentId = validId<DocumentId>("document.snapshot-empty");
    const auto advancedDocumentId = validId<DocumentId>("document.snapshot-ahead");

    {
        PersistenceService persistence;
        configureService(persistence, path, snapshotDirectory);
        DocumentStore documents;
        REQUIRE(documents.addDocument(projectId, emptyDocumentId).hasValue());
        REQUIRE(documents.addDocument(projectId, advancedDocumentId).hasValue());

        auto empty = documents.snapshot(emptyDocumentId);
        REQUIRE(empty.hasValue());
        auto captured = persistence.captureSnapshot(
            validId<SnapshotId>("snapshot.empty"), empty.value());
        REQUIRE(captured.hasValue());
        CHECK(captured.value().journalSequence == 0U);

        TransactionManager memoryOnly(documents);
        auto transaction = memoryOnly.begin(
            validId<TransactionId>("transaction.snapshot-ahead"), advancedDocumentId);
        REQUIRE(transaction.hasValue());
        REQUIRE(transaction.value()
                    ->createObject(ObjectRecord {
                        validId<ObjectId>("object.snapshot-ahead"),
                        validId<ObjectTypeId>("kernel.persistence.snapshot"),
                        Value {"not-journaled"}})
                    .hasValue());
        REQUIRE(transaction.value()->commit().hasValue());
        auto advanced = documents.snapshot(advancedDocumentId);
        REQUIRE(advanced.hasValue());
        auto rejected = persistence.captureSnapshot(
            validId<SnapshotId>("snapshot.ahead"), advanced.value());
        REQUIRE_FALSE(rejected.hasValue());
        CHECK(std::string(rejected.error().code.value())
              == "Persistence.SnapshotRevisionNotJournaled");
        CHECK_FALSE(std::filesystem::exists(
            snapshotDirectory / "snapshot.ahead.snapshot"));
    }
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
}

TEST_CASE("PersistenceService restores a snapshot and replays only its journal tail", "[persistence][recovery]")
{
    const auto path = uniqueDatabasePath();
    const auto snapshotDirectory = uniqueSnapshotDirectory();
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
    const auto projectId = validId<ProjectId>("project.recovery");
    const auto documentId = validId<DocumentId>("document.recovery");
    const auto objectId = validId<ObjectId>("object.recovery");

    {
        PersistenceService persistence;
        configureService(persistence, path, snapshotDirectory);
        DocumentStore documents;
        REQUIRE(documents.addDocument(projectId, documentId).hasValue());
        TransactionManager transactions(documents, &persistence);

        auto first = transactions.begin(
            validId<TransactionId>("transaction.recovery.1"), documentId);
        REQUIRE(first.hasValue());
        REQUIRE(first.value()
                    ->createObject(ObjectRecord {
                        objectId,
                        validId<ObjectTypeId>("kernel.persistence.recovery"),
                        Value {"snapshot-state"}})
                    .hasValue());
        REQUIRE(first.value()->collectEvent(lasercnc::messaging::PendingDomainEvent {
            validId<EventName>("kernel.recovery.history"),
            Version {1U, 0U, 0U},
            objectId,
            Value {"must-not-republish"}}).hasValue());
        REQUIRE(first.value()->commit().hasValue());
        auto snapshotState = documents.snapshot(documentId);
        REQUIRE(snapshotState.hasValue());
        REQUIRE(persistence
                    .captureSnapshot(
                        validId<SnapshotId>("snapshot.recovery"),
                        snapshotState.value())
                    .hasValue());

        auto second = transactions.begin(
            validId<TransactionId>("transaction.recovery.2"), documentId);
        REQUIRE(second.hasValue());
        REQUIRE(second.value()->replaceObjectData(objectId, Value {"journal-tail"}).hasValue());
        REQUIRE(second.value()->touchRevision(RevisionScope::Cam).hasValue());
        REQUIRE(second.value()->commit().hasValue());
    }

    {
        PersistenceService verifier;
        configureService(verifier, path, snapshotDirectory);
        auto recovered = verifier.recover();
        REQUIRE(recovered.hasValue());
        REQUIRE(recovered.value().documents.size() == 1U);
        CHECK(recovered.value().latestJournalSequence == 2U);
        CHECK(recovered.value().journalRecordsReplayed == 1U);
        REQUIRE(recovered.value().documents.front().objects.size() == 1U);
        CHECK(recovered.value().documents.front().objects.front().data
              == Value {"journal-tail"});
    }

    {
        lasercnc::kernel::AppKernel kernel;
        auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(backend.hasValue());
        auto snapshots = FilesystemSnapshotStore::create(
            FilesystemSnapshotStoreOptions {snapshotDirectory, 1024U * 1024U});
        REQUIRE(snapshots.hasValue());
        REQUIRE(kernel.persistence()
                    .configure(
                        std::move(backend).value(),
                        std::make_shared<JsonconsAdapter>(),
                        std::make_shared<Sha256HashService>(),
                        std::move(snapshots).value())
                    .hasValue());
        std::size_t delivered = 0U;
        auto subscription = kernel.events().subscribe(
            validId<SubscriptionId>("subscription.recovery"),
            lasercnc::messaging::EventFilter {
                lasercnc::messaging::EventKind::Domain,
                validId<EventName>("kernel.recovery.history")},
            lasercnc::messaging::DeliveryMode::Immediate,
            [&delivered](const lasercnc::messaging::EventEnvelope&) { ++delivered; });
        REQUIRE(subscription.hasValue());
        REQUIRE(kernel.bootstrap().hasValue());
        CHECK(delivered == 0U);
        auto restored = kernel.documents().snapshot(documentId);
        REQUIRE(restored.hasValue());
        const auto* object = restored.value().objects().find(objectId);
        REQUIRE(object != nullptr);
        CHECK(object->data == Value {"journal-tail"});
        CHECK(restored.value().revisions().at(RevisionScope::Document) == Revision {2U});
        CHECK(restored.value().revisions().at(RevisionScope::Cam) == Revision {1U});
        REQUIRE(kernel.shutdown().hasValue());
    }
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
}

TEST_CASE("Recovery keeps staggered document snapshots on one project revision chain", "[persistence][snapshot][recovery]")
{
    const auto path = uniqueDatabasePath();
    const auto snapshotDirectory = uniqueSnapshotDirectory();
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
    const auto projectId = validId<ProjectId>("project.recovery-multi");
    const auto firstDocumentId = validId<DocumentId>("document.recovery-a");
    const auto secondDocumentId = validId<DocumentId>("document.recovery-b");
    const auto firstObjectId = validId<ObjectId>("object.recovery-a");
    const auto secondObjectId = validId<ObjectId>("object.recovery-b");

    {
        PersistenceService persistence;
        configureService(persistence, path, snapshotDirectory);
        DocumentStore documents;
        REQUIRE(documents.addDocument(projectId, firstDocumentId).hasValue());
        REQUIRE(documents.addDocument(projectId, secondDocumentId).hasValue());
        TransactionManager transactions(documents, &persistence);
        const auto create = [&](const char* transactionId,
                                const DocumentId& documentId,
                                const ObjectId& objectId,
                                const char* value) {
            auto transaction = transactions.begin(
                validId<TransactionId>(transactionId), documentId);
            REQUIRE(transaction.hasValue());
            REQUIRE(transaction.value()
                        ->createObject(ObjectRecord {
                            objectId,
                            validId<ObjectTypeId>("kernel.persistence.recovery"),
                            Value {value}})
                        .hasValue());
            REQUIRE(transaction.value()->commit().hasValue());
        };
        const auto update = [&](const char* transactionId,
                                const DocumentId& documentId,
                                const ObjectId& objectId,
                                const char* value) {
            auto transaction = transactions.begin(
                validId<TransactionId>(transactionId), documentId);
            REQUIRE(transaction.hasValue());
            REQUIRE(transaction.value()->replaceObjectData(objectId, Value {value}).hasValue());
            REQUIRE(transaction.value()->commit().hasValue());
        };

        create("transaction.recovery-multi.1", firstDocumentId, firstObjectId, "a1");
        create("transaction.recovery-multi.2", secondDocumentId, secondObjectId, "b1");
        auto firstDocument = documents.snapshot(firstDocumentId);
        REQUIRE(firstDocument.hasValue());
        auto firstSnapshot = persistence.captureSnapshot(
            validId<SnapshotId>("snapshot.recovery-a"), firstDocument.value());
        REQUIRE(firstSnapshot.hasValue());
        CHECK(firstSnapshot.value().journalSequence == 2U);

        update("transaction.recovery-multi.3", firstDocumentId, firstObjectId, "a2");
        auto secondDocument = documents.snapshot(secondDocumentId);
        REQUIRE(secondDocument.hasValue());
        auto secondSnapshot = persistence.captureSnapshot(
            validId<SnapshotId>("snapshot.recovery-b"), secondDocument.value());
        REQUIRE(secondSnapshot.hasValue());
        CHECK(secondSnapshot.value().journalSequence == 3U);
        update("transaction.recovery-multi.4", secondDocumentId, secondObjectId, "b2");
    }

    {
        PersistenceService reopened;
        configureService(reopened, path, snapshotDirectory);
        auto recovered = reopened.recover();
        REQUIRE(recovered.hasValue());
        REQUIRE(recovered.value().documents.size() == 2U);
        CHECK(recovered.value().latestJournalSequence == 4U);
        CHECK(recovered.value().journalRecordsReplayed == 2U);
        for(const auto& document : recovered.value().documents) {
            CHECK(document.revisions.at(RevisionScope::Project) == Revision {4U});
            REQUIRE(document.objects.size() == 1U);
            if(document.documentId == firstDocumentId) {
                CHECK(document.objects.front().data == Value {"a2"});
            } else {
                CHECK(document.documentId == secondDocumentId);
                CHECK(document.objects.front().data == Value {"b2"});
            }
        }
    }
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
}

TEST_CASE("Crash recovery fails closed on journal gaps", "[persistence][recovery]")
{
    const auto path = uniqueDatabasePath();
    const auto snapshotDirectory = uniqueSnapshotDirectory();
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
    const RevisionSet zero;
    const RevisionSet one {
        Revision {1U}, Revision {1U}, Revision {1U}, Revision {}, Revision {}, Revision {}};
    const RevisionSet two {
        Revision {2U}, Revision {2U}, Revision {2U}, Revision {}, Revision {}, Revision {}};

    {
        PersistenceService persistence;
        configureService(persistence, path, snapshotDirectory);
        REQUIRE(persistence.append(
            commit("transaction.recovery-gap.1", zero, one, "one")).hasValue());
        REQUIRE(persistence.append(
            commit("transaction.recovery-gap.2", one, two, "two")).hasValue());
    }
    {
        auto tamper = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(tamper.hasValue());
        const std::array parameters {Value {"transaction.recovery-gap.1"}};
        REQUIRE(tamper.value()
                    ->execute(
                        "DELETE FROM state_journal WHERE transaction_id=?",
                        parameters)
                    .hasValue());
    }
    {
        PersistenceService reopened;
        configureService(reopened, path, snapshotDirectory);
        auto recovered = reopened.recover();
        REQUIRE_FALSE(recovered.hasValue());
        CHECK(std::string(recovered.error().code.value())
              == "Persistence.JournalSequenceGap");
    }
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
}

TEST_CASE("Crash recovery validates object before state", "[persistence][recovery]")
{
    const auto path = uniqueDatabasePath();
    const auto snapshotDirectory = uniqueSnapshotDirectory();
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
    const RevisionSet zero;
    const RevisionSet one {
        Revision {1U}, Revision {1U}, Revision {1U}, Revision {}, Revision {}, Revision {}};
    const RevisionSet two {
        Revision {2U}, Revision {2U}, Revision {2U}, Revision {}, Revision {}, Revision {}};
    {
        PersistenceService persistence;
        configureService(persistence, path, snapshotDirectory);
        REQUIRE(persistence.append(
            commit("transaction.replay-conflict.1", zero, one, "one")).hasValue());
        REQUIRE(persistence.append(
            commit("transaction.replay-conflict.2", one, two, "two")).hasValue());
        auto recovered = persistence.recover();
        REQUIRE_FALSE(recovered.hasValue());
        CHECK(std::string(recovered.error().code.value())
              == "Persistence.ReplayObjectConflict");
    }
    removeDatabase(path);
    removeSnapshotDirectory(snapshotDirectory);
}

TEST_CASE("PersistenceService rolls back migration exceptions and rejects newer schemas", "[persistence][migration]")
{
    auto throwing = std::make_unique<ThrowingBackend>();
    auto* observed = throwing.get();
    PersistenceService exceptionService;
    REQUIRE(exceptionService
                .configure(
                    std::move(throwing),
                    std::make_shared<JsonconsAdapter>(),
                    std::make_shared<Sha256HashService>())
                .hasValue());
    auto failed = exceptionService.initialize();
    REQUIRE_FALSE(failed.hasValue());
    CHECK(std::string(failed.error().code.value()) == "Persistence.InitializeFailed");
    CHECK(observed->begins == 1U);
    CHECK(observed->rollbacks == 1U);
    CHECK_FALSE(observed->active);

    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    {
        auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(backend.hasValue());
        REQUIRE(backend.value()
                    ->execute(
                        "CREATE TABLE schema_migrations("
                        "version INTEGER PRIMARY KEY NOT NULL,applied_at TEXT NOT NULL)")
                    .hasValue());
        const std::array parameters {Value {std::int64_t {3}}, Value {"future"}};
        REQUIRE(backend.value()
                    ->execute(
                        "INSERT INTO schema_migrations(version,applied_at) VALUES(?,?)",
                        parameters)
                    .hasValue());

        PersistenceService newerSchema;
        REQUIRE(newerSchema
                    .configure(
                        std::move(backend).value(),
                        std::make_shared<JsonconsAdapter>(),
                        std::make_shared<Sha256HashService>())
                    .hasValue());
        auto rejected = newerSchema.initialize();
        REQUIRE_FALSE(rejected.hasValue());
        CHECK(std::string(rejected.error().code.value())
              == "Persistence.SchemaTooNew");
        CHECK_FALSE(newerSchema.ready());
    }
    removeDatabase(path);
}

TEST_CASE("TransactionManager persists write-ahead journal before the memory swap", "[persistence][transaction]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    const auto projectId = validId<ProjectId>("project.write-ahead");
    const auto documentId = validId<DocumentId>("document.write-ahead");
    {
        DocumentStore documents;
        REQUIRE(documents.addDocument(projectId, documentId).hasValue());
        auto serializer = std::make_shared<ReentrantSerializer>(documents, documentId);
        auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(backend.hasValue());
        PersistenceService persistence;
        REQUIRE(persistence
                    .configure(
                        std::move(backend).value(),
                        serializer,
                        std::make_shared<Sha256HashService>())
                    .hasValue());
        REQUIRE(persistence.initialize().hasValue());
        TransactionManager transactions(documents, &persistence);
        auto transaction = transactions.begin(
            validId<TransactionId>("transaction.write-ahead"), documentId);
        REQUIRE(transaction.hasValue());
        REQUIRE(transaction.value()
                    ->createObject(ObjectRecord {
                        validId<ObjectId>("object.write-ahead"),
                        validId<ObjectTypeId>("kernel.persistence.test"),
                        Value {"persisted"}})
                    .hasValue());
        REQUIRE(transaction.value()->touchRevision(RevisionScope::Geometry).hasValue());
        auto committed = transaction.value()->commit();
        REQUIRE(committed.hasValue());
        CHECK(serializer->sawOldSnapshot);

        auto records = persistence.journalAfter(documentId, 0U);
        REQUIRE(records.hasValue());
        REQUIRE(records.value().size() == 1U);
        CHECK(records.value().front().transactionId
              == validId<TransactionId>("transaction.write-ahead"));
        auto snapshot = documents.snapshot(documentId);
        REQUIRE(snapshot.hasValue());
        CHECK(snapshot.value().objects().contains(
            validId<ObjectId>("object.write-ahead")));
        CHECK(snapshot.value().revisions().at(RevisionScope::Document) == Revision {1U});
    }
    removeDatabase(path);
}

TEST_CASE("TransactionManager leaves memory unchanged when journaling fails", "[persistence][transaction]")
{
    const auto path = uniqueDatabasePath();
    removeDatabase(path);
    const auto projectId = validId<ProjectId>("project.journal-failure");
    const auto documentId = validId<DocumentId>("document.journal-failure");
    {
        DocumentStore documents;
        REQUIRE(documents.addDocument(projectId, documentId).hasValue());
        auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(backend.hasValue());
        PersistenceService persistence;
        REQUIRE(persistence
                    .configure(
                        std::move(backend).value(),
                        std::make_shared<FailingSerializer>(),
                        std::make_shared<Sha256HashService>())
                    .hasValue());
        REQUIRE(persistence.initialize().hasValue());
        TransactionManager transactions(documents, &persistence);
        auto transaction = transactions.begin(
            validId<TransactionId>("transaction.journal-failure"), documentId);
        REQUIRE(transaction.hasValue());
        REQUIRE(transaction.value()
                    ->createObject(ObjectRecord {
                        validId<ObjectId>("object.must-not-commit"),
                        validId<ObjectTypeId>("kernel.persistence.test"),
                        Value {"unsafe"}})
                    .hasValue());
        auto committed = transaction.value()->commit();
        REQUIRE_FALSE(committed.hasValue());
        CHECK(std::string(committed.error().code.value()) == "Test.SerializeFailed");

        auto snapshot = documents.snapshot(documentId);
        REQUIRE(snapshot.hasValue());
        CHECK(snapshot.value().objects().empty());
        CHECK(snapshot.value().revisions().at(RevisionScope::Document) == Revision {0U});
        CHECK(persistence.journalAfter(documentId, 0U).value().empty());
    }
    removeDatabase(path);
}

TEST_CASE("AppKernel initializes and freezes configured persistence", "[kernel][persistence]")
{
    const auto path = uniqueDatabasePath();
    const auto latePath = uniqueDatabasePath();
    removeDatabase(path);
    removeDatabase(latePath);
    {
        lasercnc::kernel::AppKernel kernel;
        auto backend = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(backend.hasValue());
        REQUIRE(kernel.persistence()
                    .configure(
                        std::move(backend).value(),
                        std::make_shared<JsonconsAdapter>(),
                        std::make_shared<Sha256HashService>())
                    .hasValue());
        REQUIRE(kernel.bootstrap().hasValue());
        CHECK(kernel.persistence().ready());
        CHECK(kernel.persistence().frozen());

        auto lateBackend = SqlitePersistenceBackend::open(
            SqliteConnectionOptions {latePath});
        REQUIRE(lateBackend.hasValue());
        auto late = kernel.persistence().configure(
            std::move(lateBackend).value(),
            std::make_shared<JsonconsAdapter>(),
            std::make_shared<Sha256HashService>());
        REQUIRE_FALSE(late.hasValue());
        CHECK(std::string(late.error().code.value())
              == "Persistence.ConfigurationFrozen");
        REQUIRE(kernel.shutdown().hasValue());
    }
    removeDatabase(path);
    removeDatabase(latePath);
}
