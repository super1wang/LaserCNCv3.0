#include <lasercnc/kernel/app_kernel.hpp>
#include "persistence_fixture.hpp"
#include "fault_injecting_backend.hpp"
#include "fault_injecting_data_plane.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

using namespace lasercnc::foundation;
using namespace lasercnc::kernel;
using namespace lasercnc::persistence;
using namespace lasercnc::infrastructure;
using namespace lasercnc::test;

namespace {

template<typename Id>
Id id(const char* text) { return Id::create(text).value(); }

template<typename Host>
concept HostSavesProject = requires(Host& host, const ProjectId& project) {
    host.persistence().saveProjectLifecycle(project, ProjectPersistenceState::Open);
};
template<typename Host>
concept HostMigratesProjects = requires(Host& host, std::span<const ProjectId> projects) {
    host.persistence().completeProjectCatalogMigration(projects);
};
static_assert(!HostSavesProject<AppKernel> && !HostSavesProject<const AppKernel>);
static_assert(!HostMigratesProjects<AppKernel> && !HostMigratesProjects<const AppKernel>);

class FaultSerializer final : public IValueSerializer {
public:
    void arm(bool reading, bool throws) { reading_ = reading; throws_ = throws; armed_ = true; hits = 0U; }
    Result<std::string> serialize(const Value& value) const override
    {
        if(armed_ && !reading_) {
            armed_ = false; ++hits;
            if(throws_) { throw std::runtime_error("Injected project serialization exception"); }
            return Result<std::string>::failure(makeError("Test.ProjectSerializationFailure",
                ErrorCategory::Infrastructure, "Injected project serialization failure"));
        }
        return delegate_.serialize(value);
    }
    Result<Value> deserialize(std::string_view payload) const override
    {
        if(armed_ && reading_) {
            armed_ = false; ++hits;
            if(throws_) { throw std::runtime_error("Injected project deserialization exception"); }
            return Result<Value>::failure(makeError("Test.ProjectSerializationFailure",
                ErrorCategory::Infrastructure, "Injected project deserialization failure"));
        }
        return delegate_.deserialize(payload);
    }
    mutable unsigned int hits{0U};
private:
    JsonconsAdapter delegate_;
    bool reading_{false};
    bool throws_{false};
    mutable bool armed_{false};
};

struct Fixture final {
    std::filesystem::path database;
    std::unique_ptr<PersistenceService> service;
    std::unique_ptr<SqlitePersistenceBackend> observer;
    FaultInjectingBackend* faults{};
    std::shared_ptr<FaultHashService> hashes;

    explicit Fixture(bool snapshots = false, std::shared_ptr<IValueSerializer> serializer = nullptr)
    {
        static std::atomic_ullong sequence{0U};
        const auto directory = std::filesystem::path{LCNC_STRESS_TEST_ROOT} / "project-catalog"
            / (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
               + "-" + std::to_string(sequence.fetch_add(1U)));
        std::filesystem::create_directories(directory);
        database = directory / "state.db";
        auto opened = SqlitePersistenceBackend::open({database});
        REQUIRE(opened.hasValue());
        auto injected = std::make_unique<FaultInjectingBackend>(std::move(opened).value());
        faults = injected.get();
        hashes = std::make_shared<FaultHashService>(std::make_shared<Sha256HashService>());
        service = std::make_unique<PersistenceService>();
        std::unique_ptr<lasercnc::platform::ISnapshotStore> store;
        if(snapshots) {
            auto openedStore = FilesystemSnapshotStore::create({directory / "snapshots"});
            REQUIRE(openedStore.hasValue());
            store = std::move(openedStore).value();
        }
        if(!serializer) { serializer = std::make_shared<JsonconsAdapter>(); }
        REQUIRE(service->configure(std::move(injected), std::move(serializer), hashes, std::move(store)).hasValue());
        REQUIRE(service->initialize().hasValue());
        auto separate = SqlitePersistenceBackend::open({database});
        REQUIRE(separate.hasValue());
        observer = std::move(separate).value();
    }

    std::vector<lasercnc::platform::PersistenceRow> rows()
    {
        auto result = observer->query("SELECT * FROM project_catalog ORDER BY project_id");
        REQUIRE(result.hasValue());
        return result.value();
    }

    std::array<ProjectId, 2U> seedLegacy()
    {
        const std::array projects{id<ProjectId>("project.legacy.a"), id<ProjectId>("project.legacy.b")};
        REQUIRE(service->saveDocumentLifecycle(projects[0], id<DocumentId>("document.legacy.a"),
            DocumentPersistenceState::Open).hasValue());
        REQUIRE(service->saveDocumentLifecycle(projects[1], id<DocumentId>("document.legacy.b"),
            DocumentPersistenceState::Detached).hasValue());
        return projects;
    }
};

} // namespace

TEST_CASE("Project catalog persists independent empty projects across reconnect", "[persistence][project][st1a]")
{
    Fixture fixture;
    auto pending = fixture.service->projectCatalogMigrationPending();
    REQUIRE(pending.hasValue());
    CHECK(pending.value());
    auto unreadable = fixture.service->projectCatalog();
    REQUIRE_FALSE(unreadable.hasValue());
    CHECK(std::string(unreadable.error().code.value()) == "Persistence.ProjectCatalogMigrationRequired");
    const auto a = id<ProjectId>("project.empty.a");
    const auto b = id<ProjectId>("project.empty.b");
    auto premature = fixture.service->saveProjectLifecycle(a, ProjectPersistenceState::Open);
    REQUIRE_FALSE(premature.hasValue());
    CHECK(std::string(premature.error().code.value()) == "Persistence.ProjectCatalogMigrationRequired");
    CHECK(fixture.rows().empty());
    REQUIRE(fixture.service->completeProjectCatalogMigration({}).hasValue());
    REQUIRE(fixture.service->saveProjectLifecycle(b, ProjectPersistenceState::Open).hasValue());
    REQUIRE(fixture.service->saveProjectLifecycle(a, ProjectPersistenceState::Closed).hasValue());
    auto documents = fixture.service->documentCatalog();
    REQUIRE(documents.hasValue());
    CHECK(documents.value().empty());
    fixture.service.reset();
    auto reopened = openPersistenceFixture(fixture.database);
    REQUIRE_FALSE(reopened->projectCatalogMigrationPending().value());
    auto projects = reopened->projectCatalog();
    REQUIRE(projects.hasValue());
    REQUIRE(projects.value().size() == 2U);
    CHECK(projects.value()[0].projectId == a);
    CHECK(projects.value()[0].state == ProjectPersistenceState::Closed);
    CHECK(projects.value()[1].projectId == b);
    CHECK(projects.value()[1].state == ProjectPersistenceState::Open);
    CHECK(projects.value()[0].updatedAt.time_since_epoch().count() > 0);
}

TEST_CASE("Project catalog validates states and reports interrupted transitions without rewriting", "[persistence][project][st1a]")
{
    Fixture fixture;
    REQUIRE(fixture.service->completeProjectCatalogMigration({}).hasValue());
    const auto project = id<ProjectId>("project.states");
    for(const auto state : {ProjectPersistenceState::Closed, ProjectPersistenceState::Opening,
            ProjectPersistenceState::Open, ProjectPersistenceState::Closing, ProjectPersistenceState::Failed}) {
        REQUIRE(fixture.service->saveProjectLifecycle(project, state).hasValue());
        const auto before = fixture.rows();
        auto catalog = fixture.service->projectCatalog();
        REQUIRE(catalog.hasValue());
        REQUIRE(catalog.value().size() == 1U);
        const bool interrupted = state == ProjectPersistenceState::Opening || state == ProjectPersistenceState::Closing;
        CHECK(catalog.value().front().state == (interrupted ? ProjectPersistenceState::Failed : state));
        CHECK(catalog.value().front().interruptedTransition == interrupted);
        CHECK(fixture.rows() == before);
    }
    const auto before = fixture.rows();
    for(const auto unknown : {5U, 255U}) {
        auto rejected = fixture.service->saveProjectLifecycle(project, static_cast<ProjectPersistenceState>(unknown));
        REQUIRE_FALSE(rejected.hasValue());
        CHECK(std::string(rejected.error().code.value()) == "Persistence.InvalidProjectLifecycleState");
        CHECK(fixture.rows() == before);
    }
}

TEST_CASE("Project catalog rejects tampered control columns payload versions and timestamps", "[persistence][project][st1a]")
{
    for(unsigned int mode = 0U; mode < 10U; ++mode) {
        DYNAMIC_SECTION("tamper=" << mode) {
            Fixture fixture;
            REQUIRE(fixture.service->completeProjectCatalogMigration({}).hasValue());
            auto project = id<ProjectId>("project.tamper");
            REQUIRE(fixture.service->saveProjectLifecycle(project, ProjectPersistenceState::Open).hasValue());
            if(mode < 8U) {
                const std::array statements{
                    "UPDATE project_catalog SET state='unknown'",
                    "UPDATE project_catalog SET state='closed'",
                    "UPDATE project_catalog SET payload='{}'",
                    "UPDATE project_catalog SET digest='tampered'",
                    "UPDATE project_catalog SET project_id='project.other'",
                    "UPDATE project_catalog SET updated_at_ms=updated_at_ms+1",
                    "UPDATE project_catalog SET updated_at_ms=-1",
                    "UPDATE project_catalog SET updated_at_ms=9223372036854775807"};
                REQUIRE(fixture.observer->execute(statements[mode]).hasValue());
                if(mode == 4U) { project = id<ProjectId>("project.other"); }
            } else {
                auto rows = fixture.rows();
                JsonconsAdapter serializer;
                auto decoded = serializer.deserialize(*rows.front().at("payload").getIf<std::string>());
                REQUIRE(decoded.hasValue());
                auto* object = decoded.value().getIf<Value::Object>();
                REQUIRE(object != nullptr);
                if(mode == 8U) { object->at("format") = Value{"lasercnc.project-lifecycle.v2"}; }
                else { object->erase("updatedAtMs"); }
                auto encoded = serializer.serialize(decoded.value());
                REQUIRE(encoded.hasValue());
                auto digest = fixture.hashes->digest({reinterpret_cast<const std::byte*>(encoded.value().data()), encoded.value().size()});
                REQUIRE(digest.hasValue());
                const std::array parameters{Value{encoded.value()}, Value{std::string(digest.value().value())}};
                REQUIRE(fixture.observer->execute("UPDATE project_catalog SET payload=?,digest=?", parameters).hasValue());
            }
            const auto damaged = fixture.rows();
            CHECK_FALSE(fixture.service->projectCatalog().hasValue());
            CHECK_FALSE(fixture.service->saveProjectLifecycle(project, ProjectPersistenceState::Closed).hasValue());
            CHECK(fixture.rows() == damaged);
            CHECK(fixture.service->ready());
        }
    }
}

TEST_CASE("Project migration requires exact verified legacy ownership and never reseeds", "[persistence][project][st1a]")
{
    Fixture fixture;
    const auto projects = fixture.seedLegacy();
    const auto unrelated = id<ProjectId>("project.unrelated");
    const std::array missing{projects[0]};
    const std::array duplicate{projects[0], projects[0], projects[1]};
    const std::array extra{projects[0], projects[1], unrelated};
    for(const auto candidate : {std::span<const ProjectId>{}, std::span<const ProjectId>{missing},
            std::span<const ProjectId>{duplicate}, std::span<const ProjectId>{extra}}) {
        CHECK_FALSE(fixture.service->completeProjectCatalogMigration(candidate).hasValue());
        CHECK(fixture.rows().empty());
        CHECK(fixture.service->projectCatalogMigrationPending().value());
    }
    REQUIRE(fixture.service->completeProjectCatalogMigration(projects).hasValue());
    auto catalog = fixture.service->projectCatalog();
    REQUIRE(catalog.hasValue());
    REQUIRE(catalog.value().size() == 2U);
    CHECK(catalog.value()[0].projectId == projects[0]);
    CHECK(catalog.value()[1].projectId == projects[1]);
    CHECK(catalog.value()[0].state == ProjectPersistenceState::Open);
    REQUIRE(fixture.service->saveProjectLifecycle(projects[0], ProjectPersistenceState::Closed).hasValue());
    const auto before = fixture.rows();
    const std::array later{unrelated};
    REQUIRE(fixture.service->completeProjectCatalogMigration(later).hasValue());
    CHECK(fixture.rows() == before);
}

TEST_CASE("Schema v8 upgrade retains documents until verified project migration completes", "[persistence][project][st1a]")
{
    Fixture fixture;
    const auto projects = fixture.seedLegacy();
    const auto before = fixture.service->documentCatalog();
    REQUIRE(before.hasValue());
    fixture.service.reset();
    // Reconstruct the exact preceding schema in this isolated fixture, not a user database.
    // 中文翻译：仅在隔离夹具中重建前一版 schema，不修改用户数据库。
    REQUIRE(fixture.observer->execute("DROP TABLE project_catalog").hasValue());
    REQUIRE(fixture.observer->execute("DROP TABLE project_catalog_migration").hasValue());
    REQUIRE(fixture.observer->execute("DELETE FROM schema_migrations WHERE version=9").hasValue());
    auto upgraded = openPersistenceFixture(fixture.database);
    REQUIRE(upgraded->projectCatalogMigrationPending().value());
    CHECK(fixture.rows().empty());
    auto documents = upgraded->documentCatalog();
    REQUIRE(documents.hasValue());
    REQUIRE(documents.value().size() == before.value().size());
    CHECK(documents.value()[1].state == DocumentPersistenceState::Detached);
    REQUIRE(upgraded->completeProjectCatalogMigration(projects).hasValue());
    CHECK(upgraded->projectCatalog().value().size() == 2U);
}

TEST_CASE("Project migration includes journal only and snapshot only ownership", "[persistence][project][st1a]")
{
    using namespace lasercnc::state;
    using namespace lasercnc::runtime;
    Fixture fixture(true);
    const auto journalProject = id<ProjectId>("project.journal-only");
    const auto snapshotProject = id<ProjectId>("project.snapshot-only");
    const auto object = id<ObjectId>("object.legacy");
    const TransactionCommit commit{id<TransactionId>("transaction.legacy"), journalProject,
        id<DocumentId>("document.journal-only"), RevisionSet{},
        RevisionSet{Revision{1U}, Revision{1U}, Revision{}, Revision{}, Revision{}, Revision{}},
        {ObjectChange{ObjectChangeKind::Created, object, std::nullopt,
            ObjectRecord{object, id<ObjectTypeId>("kernel.project.test"), Value{"legacy"}}}}, {}, {}};
    REQUIRE(fixture.service->append(commit).hasValue());
    DocumentStore store;
    const auto snapshotDocument = id<DocumentId>("document.snapshot-only");
    REQUIRE(store.addDocument(snapshotProject, snapshotDocument).hasValue());
    auto image = store.snapshot(snapshotDocument);
    REQUIRE(image.hasValue());
    REQUIRE(fixture.service->captureSnapshot(id<SnapshotId>("snapshot.legacy"), image.value()).hasValue());
    REQUIRE(fixture.service->documentCatalog().value().empty());
    auto recovered = fixture.service->recover();
    REQUIRE(recovered.hasValue());
    REQUIRE(recovered.value().documents.size() == 2U);
    const std::array partial{journalProject};
    CHECK_FALSE(fixture.service->completeProjectCatalogMigration(partial).hasValue());
    CHECK(fixture.rows().empty());
    const std::array all{journalProject, snapshotProject};
    REQUIRE(fixture.service->completeProjectCatalogMigration(all).hasValue());
    CHECK(fixture.service->projectCatalog().value().size() == 2U);
}

TEST_CASE("Project catalog read faults preserve durable state and permit a clean retry", "[persistence][project][st1a][fault-matrix]")
{
    for(const bool hash : {false, true}) {
        for(const bool throws : {false, true}) {
            DYNAMIC_SECTION("hash=" << hash << " throws=" << throws) {
                Fixture fixture;
                REQUIRE(fixture.service->completeProjectCatalogMigration({}).hasValue());
                REQUIRE(fixture.service->saveProjectLifecycle(id<ProjectId>("project.read"), ProjectPersistenceState::Open).hasValue());
                const auto before = fixture.rows();
                if(hash) { fixture.hashes->arm("lasercnc.project-lifecycle.v1", 1U, throws); }
                else { fixture.faults->arm(BackendPoint::Query, "ORDER BY project_id", 1U, throws); }
                CHECK_FALSE(fixture.service->projectCatalog().hasValue());
                CHECK((hash ? fixture.hashes->hits : fixture.faults->hits) == 1U);
                CHECK(fixture.rows() == before);
                CHECK(fixture.service->ready());
                REQUIRE(fixture.service->projectCatalog().hasValue());
            }
        }
    }
}

TEST_CASE("Project catalog serialization failures never install partial metadata", "[persistence][project][st1a][fault-matrix]")
{
    for(const bool reading : {false, true}) {
        for(const bool throws : {false, true}) {
            DYNAMIC_SECTION("reading=" << reading << " throws=" << throws) {
                auto serializer = std::make_shared<FaultSerializer>();
                Fixture fixture(false, serializer);
                const auto project = id<ProjectId>("project.serializer");
                REQUIRE(fixture.service->completeProjectCatalogMigration({}).hasValue());
                REQUIRE(fixture.service->saveProjectLifecycle(project, ProjectPersistenceState::Open).hasValue());
                const auto before = fixture.rows();
                serializer->arm(reading, throws);
                if(reading) { CHECK_FALSE(fixture.service->projectCatalog().hasValue()); }
                else { CHECK_FALSE(fixture.service->saveProjectLifecycle(project, ProjectPersistenceState::Closed).hasValue()); }
                CHECK(serializer->hits == 1U);
                CHECK(fixture.rows() == before);
                CHECK(fixture.service->ready());
                REQUIRE(fixture.service->projectCatalog().hasValue());
                REQUIRE(fixture.service->saveProjectLifecycle(project, ProjectPersistenceState::Closed).hasValue());
            }
        }
    }
}

TEST_CASE("Project catalog migration and writes roll back database and hash faults", "[persistence][project][st1a][fault-matrix]")
{
    for(const bool migration : {false, true}) {
        for(const bool throws : {false, true}) {
            for(unsigned int stage = 0U; stage < (migration ? 7U : 6U); ++stage) {
                DYNAMIC_SECTION("migration=" << migration << " throws=" << throws << " stage=" << stage) {
                    Fixture fixture;
                    const auto projects = fixture.seedLegacy();
                    if(!migration) { REQUIRE(fixture.service->completeProjectCatalogMigration(projects).hasValue()); }
                    const auto before = fixture.rows();
                    switch(stage) {
                    case 0U: fixture.faults->arm(BackendPoint::Begin, "", 1U, throws); break;
                    case 1U: fixture.faults->arm(BackendPoint::Query, "project_catalog_migration", 1U, throws); break;
                    case 2U: fixture.faults->arm(BackendPoint::Query,
                        migration ? "SELECT project_id FROM document_catalog" : "WHERE project_id=?", 1U, throws); break;
                    case 3U: fixture.faults->arm(BackendPoint::Execute, "INSERT INTO project_catalog(", migration ? 2U : 1U, throws); break;
                    case 4U: fixture.hashes->arm("lasercnc.project-lifecycle.v1", 2U, throws); break;
                    case 5U: fixture.faults->arm(BackendPoint::Commit, "", 1U, throws); break;
                    default: fixture.faults->arm(BackendPoint::Execute, "UPDATE project_catalog_migration", 1U, throws); break;
                    }
                    auto execute = [&]() {
                        return migration ? fixture.service->completeProjectCatalogMigration(projects)
                            : fixture.service->saveProjectLifecycle(projects[0], ProjectPersistenceState::Closed);
                    };
                    REQUIRE_FALSE(execute().hasValue());
                    CHECK((stage == 4U ? fixture.hashes->hits : fixture.faults->hits) == 1U);
                    CHECK(fixture.service->ready());
                    CHECK(fixture.rows() == before);
                    CHECK(fixture.service->projectCatalogMigrationPending().value() == migration);
                    REQUIRE(execute().hasValue());
                    CHECK_FALSE(fixture.service->projectCatalogMigrationPending().value());
                }
            }
        }
    }
}

TEST_CASE("Project catalog rollback failures quarantine the connection without durable partial state", "[persistence][project][st1a][fault-matrix]")
{
    for(const bool migration : {false, true}) {
        for(const bool throws : {false, true}) {
            DYNAMIC_SECTION("migration=" << migration << " rollbackThrows=" << throws) {
                Fixture fixture;
                const auto projects = fixture.seedLegacy();
                if(!migration) { REQUIRE(fixture.service->completeProjectCatalogMigration(projects).hasValue()); }
                const auto before = fixture.rows();
                fixture.faults->arm(BackendPoint::Commit, "", 1U, false);
                fixture.faults->failRollback = true;
                fixture.faults->throwRollback = throws;
                auto failed = migration ? fixture.service->completeProjectCatalogMigration(projects)
                    : fixture.service->saveProjectLifecycle(projects[0], ProjectPersistenceState::Closed);
                REQUIRE_FALSE(failed.hasValue());
                CHECK(std::string(failed.error().code.value()) == "Persistence.ProjectCatalogRollbackFailed");
                CHECK(fixture.faults->rollbackHits == 1U);
                CHECK_FALSE(fixture.service->ready());
                CHECK_FALSE(fixture.service->projectCatalog().hasValue());
                CHECK_FALSE(fixture.service->completeProjectCatalogMigration(projects).hasValue());
                CHECK_FALSE(fixture.service->saveProjectLifecycle(projects[0], ProjectPersistenceState::Open).hasValue());
                CHECK_FALSE(fixture.service->initialize().hasValue());
                CHECK(fixture.rows() == before);
                fixture.service.reset();
                auto fresh = openPersistenceFixture(fixture.database);
                CHECK(fresh->projectCatalogMigrationPending().value() == migration);
                CHECK(fixture.rows() == before);
                REQUIRE(fresh->completeProjectCatalogMigration(projects).hasValue());
            }
        }
    }
}

TEST_CASE("Project schema refuses missing catalog tables or migration markers on reconnect", "[persistence][project][st1a]")
{
    for(const auto statement : {"DELETE FROM project_catalog_migration", "DROP TABLE project_catalog",
             "DROP TABLE project_catalog_migration"}) {
        DYNAMIC_SECTION(statement) {
            Fixture fixture;
            REQUIRE(fixture.service->completeProjectCatalogMigration({}).hasValue());
            REQUIRE(fixture.service->saveProjectLifecycle(id<ProjectId>("project.schema"), ProjectPersistenceState::Open).hasValue());
            REQUIRE(fixture.observer->execute(statement).hasValue());
            fixture.service.reset();
            auto backend = SqlitePersistenceBackend::open({fixture.database});
            REQUIRE(backend.hasValue());
            PersistenceService reopened;
            REQUIRE(reopened.configure(std::move(backend).value(), std::make_shared<JsonconsAdapter>(),
                std::make_shared<Sha256HashService>()).hasValue());
            CHECK_FALSE(reopened.initialize().hasValue());
            CHECK_FALSE(reopened.ready());
        }
    }
}
