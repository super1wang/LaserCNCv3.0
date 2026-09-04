#include <lasercnc/kernel/app_kernel.hpp>
#include <lasercnc/runtime/transaction_manager.hpp>
#include <catch2/catch_test_macros.hpp>
#include "persistence_fixture.hpp"
#include "snapshot_storage_fixture.hpp"
#include "kernel_test_module.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>

using namespace lasercnc;
namespace {
template<class T> T id(std::string value) { return T::create(std::move(value)).value(); }
std::filesystem::path root()
{
    static std::atomic_uint sequence{0U};
    auto path = std::filesystem::path{LCNC_STRESS_TEST_ROOT} / "snapshot-ordering"
        / (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-"
            + std::to_string(sequence.fetch_add(1U)));
    std::filesystem::create_directories(path);
    return path;
}
void timestamp(const std::filesystem::path& directory, const kernel::SnapshotId& key, std::int64_t value)
{
    // Only test index timestamps are adjusted; never change the machine clock.
    // 中文翻译：只调整测试索引时间，不改变机器时钟或快照认证负载。
    auto sql = infrastructure::SqlitePersistenceBackend::open({directory / "state.db"});
    REQUIRE(sql);
    REQUIRE(sql.value()->execute("UPDATE snapshot_index SET created_at_ms=? WHERE snapshot_id=?",
        std::array{foundation::Value{value}, foundation::Value{std::string(key.value())}}));
}
void touch(runtime::TransactionManager& transactions, const kernel::DocumentId& document, std::string name)
{
    auto transaction = transactions.begin(id<kernel::TransactionId>(name), document);
    REQUIRE(transaction);
    REQUIRE(transaction.value()->createObject({id<kernel::ObjectId>("object." + name),
        id<kernel::ObjectTypeId>("type.snapshot-ordering"), foundation::Value{name}}));
    REQUIRE(transaction.value()->touchRevision(state::RevisionScope::Geometry));
    auto committed = transaction.value()->commit();
    INFO((committed ? "committed" : std::string(committed.error().code.value())));
    REQUIRE(committed);
}
void configure(kernel::AppKernel& host, const std::filesystem::path& directory)
{
    REQUIRE(test::registerObjectType(host, test::valueObjectType("type.snapshot-ordering")));
    auto backend = infrastructure::SqlitePersistenceBackend::open({directory / "state.db"});
    auto files = infrastructure::FilesystemSnapshotStore::create({directory / "files", 1024U * 1024U});
    REQUIRE(backend); REQUIRE(files);
    REQUIRE(host.configurePersistence(std::move(backend).value(),
        std::make_shared<infrastructure::JsonconsAdapter>(),
        std::make_shared<infrastructure::Sha256HashService>(), std::move(files).value()));
}
void replaceSnapshotFixture(const std::filesystem::path& directory, const kernel::SnapshotId& key,
    const foundation::Value& value)
{
    const auto payload = infrastructure::JsonconsAdapter{}.serialize(value).value();
    const auto digest = infrastructure::Sha256HashService{}.digest(std::as_bytes(std::span{payload.data(), payload.size()})).value();
    {
        std::ofstream output(test::snapshotStoragePath(directory / "files", key.value()), std::ios::binary | std::ios::trunc);
        output << test::snapshotStorageEnvelope(key.value(), payload);
        REQUIRE(output.good());
    }
    auto sql = infrastructure::SqlitePersistenceBackend::open({directory / "state.db"});
    REQUIRE(sql);
    REQUIRE(sql.value()->execute("UPDATE snapshot_index SET digest=?,payload_size=? WHERE snapshot_id=?",
        std::array{foundation::Value{std::string(digest.value())}, foundation::Value{static_cast<std::int64_t>(payload.size())},
            foundation::Value{std::string(key.value())}}));
}
void rejectRestart(const std::filesystem::path& directory, const kernel::DocumentId& document,
    std::string_view expected)
{
    kernel::AppKernel restarted;
    configure(restarted, directory);
    auto booted = restarted.bootstrap();
    REQUIRE_FALSE(booted);
    const auto* cause = &booted.error();
    while(cause->cause) { cause = cause->cause.get(); }
    CHECK(std::string(cause->code.value()) == std::string(expected));
    CHECK(restarted.state() == kernel::AppKernelState::Failed);
    CHECK_FALSE(restarted.documentRuntime().accepting());
    CHECK_FALSE(restarted.documents().contains(document));
}
}

TEST_CASE("Snapshot ordering prioritizes journal watermarks across clock rollback and projects", "[snapshot-ordering]")
{
    const auto directory = root();
    const auto p = id<kernel::ProjectId>("project.order");
    const auto q = id<kernel::ProjectId>("project.other");
    const auto a = id<kernel::DocumentId>("document.Case");
    const auto b = id<kernel::DocumentId>("document.case");
    const auto c = id<kernel::DocumentId>("document.other");
    state::DocumentStore documents;
    REQUIRE(documents.addDocument(p, a)); REQUIRE(documents.addDocument(p, b));
    REQUIRE(documents.addDocument(q, c));
    auto service = test::openPersistenceFixture(directory / "state.db", directory / "files");
    runtime::TransactionManager transactions(documents, service.get());
    touch(transactions, a, "transaction.a1");
    const auto old = id<kernel::SnapshotId>("snapshot.z.old");
    REQUIRE(service->captureSnapshot(old, documents.snapshot(a).value()));
    timestamp(directory, old, 9000);
    touch(transactions, b, "transaction.b1");
    touch(transactions, c, "transaction.c1");
    touch(transactions, a, "transaction.a2");
    const std::array keys{id<kernel::SnapshotId>("snapshot.a.new"),
        id<kernel::SnapshotId>("snapshot.b.new"), id<kernel::SnapshotId>("snapshot.c.new")};
    const std::array ids{a, b, c};
    for(std::size_t index = 0U; index < ids.size(); ++index) {
        auto captured = service->captureSnapshot(keys[index], documents.snapshot(ids[index]).value());
        REQUIRE(captured);
        CHECK(captured.value().journalSequence == 4U);
        timestamp(directory, keys[index], 1000);
    }
    for(int reopen = 0; reopen < 2; ++reopen) {
        for(std::size_t index = 0U; index < ids.size(); ++index) {
            auto latest = service->latestSnapshot(ids[index]);
            REQUIRE(latest); REQUIRE(latest.value());
            CHECK(latest.value()->snapshotId == keys[index]);
            CHECK(latest.value()->revisions == documents.snapshot(ids[index]).value().revisions());
        }
        auto recovered = service->recover();
        REQUIRE(recovered);
        CHECK(recovered.value().latestJournalSequence == 4U);
        CHECK(recovered.value().journalRecordsReplayed == 0U);
        REQUIRE(recovered.value().documents.size() == 3U);
        for(const auto& image : recovered.value().documents) {
            const auto expected = documents.snapshot(image.documentId).value();
            CHECK(image.projectId == expected.projectId());
            CHECK(image.revisions == expected.revisions());
            CHECK(image.objects.size() == (image.documentId == a ? 2U : 1U));
            for(const auto& object : image.objects) {
                CHECK(object.type == id<kernel::ObjectTypeId>("type.snapshot-ordering"));
                CHECK(std::string(object.id.value()) == "object." + *object.data.getIf<std::string>());
            }
        }
        service.reset();
        service = test::openPersistenceFixture(directory / "state.db", directory / "files");
    }
}

TEST_CASE("Snapshot ordering defines deterministic ties without promising last capture time", "[snapshot-ordering]")
{
    const auto directory = root();
    state::DocumentStore documents;
    const auto document = id<kernel::DocumentId>("document.tie");
    REQUIRE(documents.addDocument(id<kernel::ProjectId>("project.tie"), document));
    auto service = test::openPersistenceFixture(directory / "state.db", directory / "files");
    const auto first = id<kernel::SnapshotId>("snapshot.z");
    const auto last = id<kernel::SnapshotId>("snapshot.a");
    REQUIRE(service->captureSnapshot(first, documents.snapshot(document).value()));
    REQUIRE(service->captureSnapshot(last, documents.snapshot(document).value()));
    timestamp(directory, first, 2000); timestamp(directory, last, 1000);
    CHECK(service->latestSnapshot(document).value()->snapshotId == first);
    timestamp(directory, last, 2000);
    CHECK(service->latestSnapshot(document).value()->snapshotId == first);
    service.reset();
    service = test::openPersistenceFixture(directory / "state.db", directory / "files");
    CHECK(service->latestSnapshot(document).value()->snapshotId == first);
    auto recovered = service->recover();
    REQUIRE(recovered); REQUIRE(recovered.value().documents.size() == 1U);
    CHECK(recovered.value().documents.front().revisions == state::RevisionSet{});
    CHECK(recovered.value().documents.front().objects.empty());
    // Damage the selected tie winner; recovery must not silently use the valid other record.
    // 中文翻译：破坏排序选中的记录，恢复不能悄悄回退到另一条完好记录。
    REQUIRE(std::filesystem::remove(test::snapshotStoragePath(directory / "files", first.value())));
    CHECK_FALSE(service->latestSnapshot(document));
    CHECK_FALSE(service->recover());
}

TEST_CASE("Snapshot anchor reads reject forged watermarks and untrusted journal history", "[snapshot-anchor]")
{
    for(const int fault : {0, 1, 2, 3}) {
        DYNAMIC_SECTION("anchor fault " << fault) {
            const auto directory = root();
            const auto project = id<kernel::ProjectId>("project.anchor");
            const auto a = id<kernel::DocumentId>("document.anchor.a");
            const auto b = id<kernel::DocumentId>("document.anchor.b");
            state::DocumentStore documents;
            REQUIRE(documents.addDocument(project, a)); REQUIRE(documents.addDocument(project, b));
            auto service = test::openPersistenceFixture(directory / "state.db", directory / "files");
            runtime::TransactionManager transactions(documents, service.get());
            touch(transactions, a, "transaction.anchor.a");
            REQUIRE(service->captureSnapshot(id<kernel::SnapshotId>("snapshot.anchor"), documents.snapshot(a).value()));
            touch(transactions, b, "transaction.anchor.b");
            auto sql = infrastructure::SqlitePersistenceBackend::open({directory / "state.db"});
            REQUIRE(sql);
            const std::array statements{
                "UPDATE snapshot_index SET journal_sequence=99",
                "UPDATE snapshot_index SET journal_sequence=0",
                "UPDATE snapshot_index SET journal_sequence=2",
                "UPDATE state_journal SET payload='damaged' WHERE sequence=1"};
            REQUIRE(sql.value()->execute(statements[static_cast<std::size_t>(fault)]));
            auto latest = service->latestSnapshot(a);
            auto recovered = service->recover();
            CHECK_FALSE(latest);
            REQUIRE_FALSE(recovered);
            const auto expected = fault == 0 ? "Persistence.SnapshotSequenceAhead"
                : fault == 3 ? "Persistence.JournalDigestMismatch" : "Persistence.SnapshotRevisionMismatch";
            CHECK(std::string(recovered.error().code.value()) == expected);
            if(!latest) { CHECK(std::string(latest.error().code.value()) == expected); }
            service.reset();
            rejectRestart(directory, a, expected);
        }
    }
}

TEST_CASE("Snapshot ordering refuses damaged newest state and immutable retry before reopening", "[snapshot-ordering]")
{
    for(const bool missing : {false, true}) {
        DYNAMIC_SECTION("missing " << missing) {
            const auto directory = root();
            state::DocumentStore documents;
            const auto document = id<kernel::DocumentId>("document.damage");
            REQUIRE(documents.addDocument(id<kernel::ProjectId>("project.damage"), document));
            auto service = test::openPersistenceFixture(directory / "state.db", directory / "files");
            runtime::TransactionManager transactions(documents, service.get());
            const auto old = id<kernel::SnapshotId>("snapshot.old");
            const auto newest = id<kernel::SnapshotId>("snapshot.new");
            REQUIRE(service->captureSnapshot(old, documents.snapshot(document).value()));
            touch(transactions, document, "transaction.damage");
            auto conflicting = service->captureSnapshot(old, documents.snapshot(document).value());
            REQUIRE_FALSE(conflicting);
            CHECK(std::string(conflicting.error().code.value()) == "Persistence.SnapshotIdentityConflict");
            auto saved = service->captureSnapshot(newest, documents.snapshot(document).value());
            REQUIRE(saved);
            timestamp(directory, old, 9000); timestamp(directory, newest, 1000);
            const auto file = test::snapshotStoragePath(directory / "files", newest.value());
            REQUIRE(std::filesystem::remove(file));
            if(!missing) {
                std::ofstream output(file, std::ios::binary);
                output << test::snapshotStorageEnvelope(newest.value(), "damaged");
                REQUIRE(output.good());
            }
            CHECK_FALSE(service->latestSnapshot(document));
            CHECK_FALSE(service->recover());
            CHECK_FALSE(service->captureSnapshot(newest, documents.snapshot(document).value()));
            CHECK(std::filesystem::exists(file) == !missing);
            if(!missing) {
                std::ifstream input(file, std::ios::binary);
                CHECK(std::string(std::istreambuf_iterator<char>{input}, {})
                    == test::snapshotStorageEnvelope(newest.value(), "damaged"));
            }
            service.reset();
            service = test::openPersistenceFixture(directory / "state.db", directory / "files");
            CHECK_FALSE(service->latestSnapshot(document));
            CHECK_FALSE(service->recover());
            // Explicit fixture repair, not production auto-repair: restore the exact saved bytes.
            // 中文翻译：测试显式还原精确原字节，不是生产自动修复或覆盖许可。
            {
                std::ofstream output(file, std::ios::binary | std::ios::trunc);
                output << test::snapshotStorageEnvelope(newest.value(), saved.value().payload);
                REQUIRE(output.good());
            }
            auto recovered = service->recover();
            REQUIRE(recovered); REQUIRE(recovered.value().documents.size() == 1U);
            CHECK(recovered.value().documents.front().revisions == saved.value().revisions);
            CHECK(service->captureSnapshot(newest, documents.snapshot(document).value()));
        }
    }
}

TEST_CASE("Snapshot anchor rejects content contradicting authenticated journal objects", "[snapshot-anchor]")
{
    for(const bool missingObject : {false, true}) {
        DYNAMIC_SECTION("missing object " << missingObject) {
            const auto directory = root();
            const auto document = id<kernel::DocumentId>("document.content-anchor");
            const auto key = id<kernel::SnapshotId>("snapshot.content-anchor");
            state::DocumentStore documents;
            REQUIRE(documents.addDocument(id<kernel::ProjectId>("project.content-anchor"), document));
            auto service = test::openPersistenceFixture(directory / "state.db", directory / "files");
            runtime::TransactionManager transactions(documents, service.get());
            touch(transactions, document, "transaction.content-anchor");
            auto captured = service->captureSnapshot(key, documents.snapshot(document).value());
            REQUIRE(captured);
            infrastructure::JsonconsAdapter codec;
            auto value = codec.deserialize(captured.value().payload).value();
            auto& objects = *value.getIf<foundation::Value::Object>()->at("objects").getIf<foundation::Value::Array>();
            REQUIRE(objects.size() == 1U);
            if(missingObject) { objects.clear(); }
            else { objects.front().getIf<foundation::Value::Object>()->at("data") = foundation::Value{"contradictory"}; }
            const auto payload = codec.serialize(value).value();
            const auto digest = infrastructure::Sha256HashService{}.digest(std::as_bytes(std::span{payload.data(), payload.size()})).value();
            {
                std::ofstream output(test::snapshotStoragePath(directory / "files", key.value()), std::ios::binary | std::ios::trunc);
                output << test::snapshotStorageEnvelope(key.value(), payload);
                REQUIRE(output.good());
            }
            auto sql = infrastructure::SqlitePersistenceBackend::open({directory / "state.db"});
            REQUIRE(sql);
            REQUIRE(sql.value()->execute("UPDATE snapshot_index SET digest=?,payload_size=?",
                std::array{foundation::Value{std::string(digest.value())}, foundation::Value{static_cast<std::int64_t>(payload.size())}}));
            auto latest = service->latestSnapshot(document);
            auto recovered = service->recover();
            CHECK_FALSE(latest); CHECK_FALSE(recovered);
            if(!latest) { CHECK(std::string(latest.error().code.value()) == "Persistence.SnapshotContentMismatch"); }
            if(!recovered) { CHECK(std::string(recovered.error().code.value()) == "Persistence.SnapshotContentMismatch"); }
            service.reset();
            rejectRestart(directory, document, "Persistence.SnapshotContentMismatch");
        }
    }
}

TEST_CASE("Snapshot history identity restores journal only snapshot only and mixed owners", "[snapshot-history-identity]")
{
    for(const int source : {0, 1, 2}) {
        DYNAMIC_SECTION("source " << source) {
            const auto directory = root();
            const auto project = id<kernel::ProjectId>("project.history");
            const auto document = id<kernel::DocumentId>("document.history");
            state::RevisionSet expected;
            {
                state::DocumentStore documents;
                REQUIRE(documents.addDocument(project, document));
                auto service = test::openPersistenceFixture(directory / "state.db", directory / "files");
                runtime::TransactionManager transactions(documents, service.get());
                if(source != 1) { touch(transactions, document, "transaction.history"); }
                if(source != 0) {
                    REQUIRE(service->captureSnapshot(id<kernel::SnapshotId>("snapshot.history"), documents.snapshot(document).value()));
                }
                expected = documents.snapshot(document).value().revisions();
                REQUIRE(service->documentCatalog()); CHECK(service->documentCatalog().value().empty());
            }
            for(int restart = 0; restart < 2; ++restart) {
                kernel::AppKernel host;
                configure(host, directory);
                // A same-owner startup declaration is restored, not a request to reset durable state.
                // 中文翻译：同归属启动声明参与恢复，不要求将持久状态重置为空文档。
                if(restart == 0) { REQUIRE(host.addDocument(project, document)); }
                REQUIRE(host.bootstrap());
                CHECK(host.documents().snapshot(document).value().projectId() == project);
                CHECK(host.documents().snapshot(document).value().revisions() == expected);
                CHECK_FALSE(host.projectRuntime().create(project));
                CHECK_FALSE(host.documentRuntime().create(project, document));
                const auto foreign = id<kernel::ProjectId>("project.foreign");
                if(restart == 0) { REQUIRE(host.projectRuntime().create(foreign)); }
                CHECK_FALSE(host.documentRuntime().create(foreign, document));
                CHECK(host.documents().snapshot(document).value().revisions() == expected);
                REQUIRE(host.shutdown());
            }
        }
    }
}

TEST_CASE("Snapshot anchor preserves journal deletion against snapshot resurrection", "[snapshot-anchor]")
{
    const auto directory = root();
    const auto document = id<kernel::DocumentId>("document.deleted-anchor");
    const auto key = id<kernel::SnapshotId>("snapshot.deleted-anchor");
    state::DocumentStore documents;
    REQUIRE(documents.addDocument(id<kernel::ProjectId>("project.deleted-anchor"), document));
    auto service = test::openPersistenceFixture(directory / "state.db", directory / "files");
    runtime::TransactionManager transactions(documents, service.get());
    touch(transactions, document, "transaction.deleted-anchor");
    auto before = service->captureSnapshot(id<kernel::SnapshotId>("snapshot.before-delete"), documents.snapshot(document).value());
    REQUIRE(before);
    auto removal = transactions.begin(id<kernel::TransactionId>("transaction.remove-anchor"), document);
    REQUIRE(removal);
    REQUIRE(removal.value()->removeObject(id<kernel::ObjectId>("object.transaction.deleted-anchor")));
    REQUIRE(removal.value()->commit());
    auto after = service->captureSnapshot(key, documents.snapshot(document).value());
    REQUIRE(after);
    CHECK(service->latestSnapshot(document)); CHECK(service->recover());
    auto value = infrastructure::JsonconsAdapter{}.deserialize(after.value().payload).value();
    const auto oldValue = infrastructure::JsonconsAdapter{}.deserialize(before.value().payload).value();
    value.getIf<foundation::Value::Object>()->at("objects") = oldValue.getIf<foundation::Value::Object>()->at("objects");
    replaceSnapshotFixture(directory, key, value);
    auto latest = service->latestSnapshot(document);
    auto recovered = service->recover();
    REQUIRE_FALSE(latest); REQUIRE_FALSE(recovered);
    CHECK(std::string(latest.error().code.value()) == "Persistence.SnapshotContentMismatch");
    CHECK(std::string(recovered.error().code.value()) == "Persistence.SnapshotContentMismatch");
    service.reset();
    rejectRestart(directory, document, "Persistence.SnapshotContentMismatch");
}

TEST_CASE("Snapshot anchor validates document ownership beyond captured watermark", "[snapshot-anchor]")
{
    const auto directory = root();
    const auto document = id<kernel::DocumentId>("document.owner-anchor");
    const auto key = id<kernel::SnapshotId>("snapshot.owner-anchor");
    state::DocumentStore documents;
    REQUIRE(documents.addDocument(id<kernel::ProjectId>("project.owner-anchor"), document));
    auto service = test::openPersistenceFixture(directory / "state.db", directory / "files");
    auto before = service->captureSnapshot(key, documents.snapshot(document).value());
    REQUIRE(before); CHECK(before.value().journalSequence == 0U);
    runtime::TransactionManager transactions(documents, service.get());
    touch(transactions, document, "transaction.owner-anchor");
    auto value = infrastructure::JsonconsAdapter{}.deserialize(before.value().payload).value();
    value.getIf<foundation::Value::Object>()->at("projectId") = foundation::Value{"project.foreign-anchor"};
    replaceSnapshotFixture(directory, key, value);
    auto sql = infrastructure::SqlitePersistenceBackend::open({directory / "state.db"});
    REQUIRE(sql);
    REQUIRE(sql.value()->execute("UPDATE snapshot_index SET project_id='project.foreign-anchor'"));
    auto latest = service->latestSnapshot(document);
    auto recovered = service->recover();
    CHECK_FALSE(latest); REQUIRE_FALSE(recovered);
    if(!latest) { CHECK(std::string(latest.error().code.value()) == "Persistence.DocumentOwnershipChanged"); }
    CHECK(std::string(recovered.error().code.value()) == "Persistence.DocumentOwnershipChanged");
    service.reset();
    rejectRestart(directory, document, "Persistence.DocumentOwnershipChanged");
}

TEST_CASE("Snapshot history identity preserves detached and removed source ownership", "[snapshot-history-identity]")
{
    for(const int source : {0, 1, 2}) {
        for(const auto lifecycle : {persistence::DocumentPersistenceState::Detached,
                persistence::DocumentPersistenceState::Removed}) {
            DYNAMIC_SECTION("source " << source << " state " << static_cast<int>(lifecycle)) {
                const auto directory = root();
                const auto project = id<kernel::ProjectId>("project.historical");
                const auto document = id<kernel::DocumentId>("document.historical");
                {
                    state::DocumentStore documents;
                    REQUIRE(documents.addDocument(project, document));
                    auto service = test::openPersistenceFixture(directory / "state.db", directory / "files");
                    runtime::TransactionManager transactions(documents, service.get());
                    if(source != 1) { touch(transactions, document, "transaction.historical"); }
                    if(source != 0) { REQUIRE(service->captureSnapshot(id<kernel::SnapshotId>("snapshot.historical"), documents.snapshot(document).value())); }
                    REQUIRE(service->saveDocumentLifecycle(project, document, lifecycle));
                }
                {
                    kernel::AppKernel host;
                    configure(host, directory);
                    REQUIRE(host.addDocument(project, document));
                    CHECK_FALSE(host.bootstrap());
                    CHECK_FALSE(host.documentRuntime().accepting());
                }
                for(int restart = 0; restart < 2; ++restart) {
                    kernel::AppKernel host;
                    configure(host, directory);
                    REQUIRE(host.bootstrap());
                    CHECK_FALSE(host.documents().contains(document));
                    CHECK_FALSE(host.documentRuntime().create(project, document));
                    CHECK_FALSE(host.documents().contains(document));
                    auto catalog = host.persistence().documentCatalog();
                    REQUIRE(catalog); REQUIRE(catalog.value().size() == 1U);
                    CHECK(catalog.value().front().projectId == project);
                    CHECK(catalog.value().front().documentId == document);
                    CHECK(catalog.value().front().state == lifecycle);
                    REQUIRE(host.shutdown());
                }
            }
        }
    }
}

TEST_CASE("Snapshot history identity rejects foreign configured ownership without replacing durable state", "[snapshot-history-identity]")
{
    for(const int source : {0, 1, 2}) {
        DYNAMIC_SECTION("source " << source) {
            const auto directory = root();
            const auto project = id<kernel::ProjectId>("project.history");
            const auto document = id<kernel::DocumentId>("document.history");
            state::RevisionSet expected;
            {
                state::DocumentStore documents;
                REQUIRE(documents.addDocument(project, document));
                auto service = test::openPersistenceFixture(directory / "state.db", directory / "files");
                runtime::TransactionManager transactions(documents, service.get());
                if(source != 1) { touch(transactions, document, "transaction.history"); }
                if(source != 0) { REQUIRE(service->captureSnapshot(id<kernel::SnapshotId>("snapshot.history"), documents.snapshot(document).value())); }
                expected = documents.snapshot(document).value().revisions();
            }
            {
                kernel::AppKernel host;
                configure(host, directory);
                REQUIRE(host.addDocument(id<kernel::ProjectId>("project.foreign"), document));
                CHECK_FALSE(host.bootstrap());
                CHECK(host.state() == kernel::AppKernelState::Failed);
                CHECK_FALSE(host.documentRuntime().accepting());
            }
            kernel::AppKernel recovered;
            configure(recovered, directory);
            REQUIRE(recovered.bootstrap());
            CHECK(recovered.documents().snapshot(document).value().projectId() == project);
            CHECK(recovered.documents().snapshot(document).value().revisions() == expected);
            REQUIRE(recovered.shutdown());
        }
    }
}
