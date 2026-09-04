#include <lasercnc/kernel/app_kernel.hpp>
#include <catch2/catch_test_macros.hpp>
#include "../../../src/runtime/catalog/catalog_clock.hpp"
#include "persistence_fixture.hpp"
#include "fault_injecting_backend.hpp"
#include "kernel_test_module.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace lasercnc;
namespace {
template<typename Id> Id id(const char* text) { return Id::create(text).value(); }
template<typename Host> concept HostResetsCatalogClock = requires(Host& host, kernel::ProjectId project) {
    host.documentRuntime().catalog_->touch(project);
    host.projectRuntime().catalog_->touch(project);
};
static_assert(!HostResetsCatalogClock<kernel::AppKernel>);
std::filesystem::path freshRoot()
{
    static std::atomic_uint sequence{0U};
    auto root = std::filesystem::path{LCNC_STRESS_TEST_ROOT} / "lifecycle-catalog"
        / (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-"
           + std::to_string(sequence.fetch_add(1U)));
    std::filesystem::create_directories(root);
    return root;
}
test::FaultInjectingBackend* configure(kernel::AppKernel& host, const std::filesystem::path& root)
{
    auto opened = infrastructure::SqlitePersistenceBackend::open({root / "state.db"});
    REQUIRE(opened);
    auto backend = std::make_unique<test::FaultInjectingBackend>(std::move(opened).value());
    auto* observer = backend.get();
    auto snapshots = infrastructure::FilesystemSnapshotStore::create({root / "snapshots", 1024U * 1024U});
    REQUIRE(snapshots);
    REQUIRE(host.configurePersistence(std::move(backend), std::make_shared<infrastructure::JsonconsAdapter>(),
        std::make_shared<infrastructure::Sha256HashService>(), std::move(snapshots).value()));
    return observer;
}
class ObservingQuery final : public runtime::IQueryHandler {
public:
    std::function<void()> observe;
    foundation::Result<foundation::Value> execute(const runtime::QueryRequest&, const runtime::QueryContext&) override
    {
        observe();
        return foundation::Result<foundation::Value>::success(foundation::Value{});
    }
};
class NullLog final : public observability::ILogService {
public:
    foundation::Result<void> write(const observability::LogRecord&) override { return foundation::Result<void>::success(); }
    foundation::Result<void> flush() override { return foundation::Result<void>::success(); }
};

template<typename Host> void projectCatalogChanges(Host& host)
{
    if constexpr(requires { host.projectRuntime().catalog(); }) {
        const auto project = id<kernel::ProjectId>("project.catalog");
        auto empty = host.projectRuntime().catalog();
        REQUIRE(empty);
        REQUIRE(host.projectRuntime().create(project));
        auto created = host.projectRuntime().catalog();
        REQUIRE(created);
        CHECK(created.value().version != empty.value().version);
        REQUIRE(created.value().entries.size() == 1U);
        CHECK(created.value().entries.front().state == runtime::ProjectLifecycleState::Open);
        REQUIRE(host.projectRuntime().close(project));
        REQUIRE(host.projectRuntime().open(project));
        auto reopened = host.projectRuntime().catalog();
        REQUIRE(reopened);
        CHECK(reopened.value().version != created.value().version);
        CHECK(reopened.value().entries.front().state == created.value().entries.front().state);
        CHECK(host.projectRuntime().catalog().value().version == reopened.value().version);
    } else { FAIL("Project catalog has no atomic versioned snapshot contract"); }
}

template<typename Host> void documentCatalogChanges(Host& host)
{
    if constexpr(requires { host.documentRuntime().catalog(); }) {
        const auto project = id<kernel::ProjectId>("project.catalog");
        const auto document = id<kernel::DocumentId>("document.catalog");
        REQUIRE(host.projectRuntime().create(project));
        auto empty = host.documentRuntime().catalog(project);
        REQUIRE(empty);
        REQUIRE(host.documentRuntime().create(project, document));
        auto created = host.documentRuntime().catalog(project);
        REQUIRE(created);
        CHECK(created.value().version != empty.value().version);
        REQUIRE(created.value().entries.size() == 1U);
        REQUIRE(host.documentRuntime().close(document));
        REQUIRE(host.documentRuntime().remove(document));
        REQUIRE(host.documentRuntime().create(project, document));
        auto recreated = host.documentRuntime().catalog(project);
        REQUIRE(recreated);
        CHECK(recreated.value().version != created.value().version);
        CHECK(recreated.value().entries.front().state == created.value().entries.front().state);
    } else { FAIL("Document catalog has no atomic versioned snapshot contract"); }
}
}

TEST_CASE("Lifecycle catalog project versions reject unchanged-state ABA", "[lifecycle-catalog]")
{
    kernel::AppKernel host;
    REQUIRE(host.bootstrap());
    projectCatalogChanges(host);
    REQUIRE(host.shutdown());
}

TEST_CASE("Lifecycle catalog document versions survive removal and recreation", "[lifecycle-catalog]")
{
    kernel::AppKernel host;
    REQUIRE(host.bootstrap());
    documentCatalogChanges(host);
    REQUIRE(host.shutdown());
}

TEST_CASE("Lifecycle catalog scopes isolate projects and exclude activity observations", "[lifecycle-catalog]")
{
    kernel::AppKernel host;
    const auto a = id<kernel::ProjectId>("project.a");
    const auto b = id<kernel::ProjectId>("project.b");
    const auto document = id<kernel::DocumentId>("document.a");
    const auto session = id<kernel::SessionId>("session.catalog");
    const auto capability = id<kernel::CapabilityId>("capability.catalog");
    const auto name = id<kernel::QueryName>("query.catalog");
    REQUIRE(host.addDocument(a, document));
    REQUIRE(host.addProject(b));
    auto handler = std::make_shared<ObservingQuery>();
    REQUIRE(host.executionServices().configure(std::make_shared<infrastructure::JsonconsAdapter>(), std::make_shared<NullLog>()));
    REQUIRE(test::registerQuery(host, {name, {1U, 0U, 0U}, test::testAnySchema("schema.arguments"),
        test::testAnySchema("schema.result"), capability}, handler));
    REQUIRE(host.capabilities().replace(session, std::array{capability}));
    const auto bootstrapped = host.bootstrap();
    if(!bootstrapped) { INFO(std::string(bootstrapped.error().code.value()) << ": " << bootstrapped.error().message); REQUIRE(bootstrapped); }
    const auto projects = host.projectRuntime().catalog(a).value();
    const auto documents = host.documentRuntime().catalog(a).value();
    handler->observe = [&] {
        CHECK(host.projectRuntime().lifecycle(a).value().activities > 0U);
        CHECK(host.documentRuntime().lifecycle(document).value().activities[1U] > 0U);
        CHECK(host.projectRuntime().catalog(a).value().version == projects.version);
        CHECK(host.documentRuntime().catalog(a).value().version == documents.version);
        CHECK_FALSE(host.projectRuntime().close(a));
        CHECK_FALSE(host.documentRuntime().close(document));
        CHECK(host.projectRuntime().catalog(a).value().version == projects.version);
        CHECK(host.documentRuntime().catalog(a).value().version == documents.version);
    };
    REQUIRE(host.execution().executeQuery({id<kernel::RequestId>("request.catalog"), {session, a, document},
        name, {1U, 0U, 0U}, foundation::Value{}, id<kernel::CorrelationId>("correlation.catalog"),
        id<kernel::TraceId>("trace.catalog")}));
    REQUIRE(host.projectRuntime().close(b));
    CHECK(host.projectRuntime().catalog(a).value().version == projects.version);
    CHECK(host.documentRuntime().catalog(a).value().version == documents.version);
    CHECK(host.documentRuntime().catalog(a).value().version != host.documentRuntime().catalog(b).value().version);
    CHECK(host.projectRuntime().catalog(a).value().version != host.documentRuntime().catalog(a).value().version);
    CHECK(host.projectRuntime().catalog().value().version != projects.version);
    REQUIRE(host.shutdown());
}

TEST_CASE("Lifecycle catalog snapshots expose coherent intermediate and failed states", "[lifecycle-catalog][fault-matrix]")
{
    for(bool projectOperation : {false, true}) {
        for(bool throws : {false, true}) {
            kernel::AppKernel host;
            auto* backend = configure(host, freshRoot());
            const auto project = id<kernel::ProjectId>("project.observed");
            const auto document = id<kernel::DocumentId>("document.observed");
            REQUIRE(host.addDocument(project, document));
            REQUIRE(host.bootstrap());
            const auto pv = host.projectRuntime().catalog(project).value().version;
            const auto dv = host.documentRuntime().catalog(project).value().version;
            std::optional<runtime::CatalogVersion> intermediate;
            const auto fragment = projectOperation ? "INSERT INTO project_catalog" : "INSERT INTO document_catalog";
            backend->beforeOperation = [&](test::BackendPoint point, std::string_view sql) {
                if(intermediate || point != test::BackendPoint::Execute || sql.find(fragment) == std::string_view::npos) { return; }
                if(projectOperation) {
                    const auto snapshot = host.projectRuntime().catalog(project).value();
                    CHECK(snapshot.version != pv);
                    CHECK(snapshot.entries.front().state == runtime::ProjectLifecycleState::Closing);
                    intermediate = snapshot.version;
                } else {
                    const auto snapshot = host.documentRuntime().catalog(project).value();
                    CHECK(snapshot.version != dv);
                    CHECK(snapshot.entries.front().state == runtime::DocumentLifecycleState::Closing);
                    intermediate = snapshot.version;
                }
            };
            backend->arm(test::BackendPoint::Execute, fragment, 1U, throws);
            if(projectOperation) { CHECK_FALSE(host.projectRuntime().close(project)); }
            else { CHECK_FALSE(host.documentRuntime().close(document)); }
            backend->beforeOperation = {};
            REQUIRE(intermediate);
            if(projectOperation) {
                const auto snapshot = host.projectRuntime().catalog(project).value();
                CHECK(snapshot.version != *intermediate);
                CHECK(snapshot.entries.front().state == runtime::ProjectLifecycleState::Failed);
                CHECK(snapshot.entries.front().error.has_value());
            } else {
                const auto snapshot = host.documentRuntime().catalog(project).value();
                CHECK(snapshot.version != *intermediate);
                CHECK(snapshot.entries.front().state == runtime::DocumentLifecycleState::Failed);
                CHECK(snapshot.entries.front().error.has_value());
            }
            REQUIRE(host.shutdown());
        }
    }
}

TEST_CASE("Lifecycle catalog partial close invalidates only affected directory scopes", "[lifecycle-catalog][fault-matrix]")
{
    for(bool throws : {false, true}) {
        kernel::AppKernel host;
        auto* backend = configure(host, freshRoot());
        const auto a = id<kernel::ProjectId>("project.a");
        const auto b = id<kernel::ProjectId>("project.b");
        REQUIRE(host.addDocument(a, id<kernel::DocumentId>("document.a")));
        REQUIRE(host.addDocument(a, id<kernel::DocumentId>("document.b")));
        REQUIRE(host.addDocument(b, id<kernel::DocumentId>("document.c")));
        REQUIRE(host.bootstrap());
        const auto oldA = host.documentRuntime().catalog(a).value().version;
        const auto oldB = host.documentRuntime().catalog(b).value().version;
        backend->arm(test::BackendPoint::Execute, "INSERT INTO document_catalog", 3U, throws);
        CHECK_FALSE(host.projectRuntime().close(a));
        const auto snapshot = host.documentRuntime().catalog(a).value();
        CHECK(snapshot.version != oldA);
        REQUIRE(snapshot.entries.size() == 2U);
        CHECK(snapshot.entries[0].state == runtime::DocumentLifecycleState::Detached);
        CHECK(snapshot.entries[1].state == runtime::DocumentLifecycleState::Failed);
        CHECK(host.documentRuntime().catalog(b).value().version == oldB);
        REQUIRE(host.shutdown());
    }
}

TEST_CASE("Lifecycle catalog new Hosts invalidate old epochs while retaining business revisions", "[lifecycle-catalog]")
{
    const auto root = freshRoot();
    const auto project = id<kernel::ProjectId>("project.restart");
    const auto document = id<kernel::DocumentId>("document.restart");
    runtime::CatalogVersion pv, dv;
    for(unsigned int attempt = 0U; attempt < 3U; ++attempt) {
        kernel::AppKernel host;
        configure(host, root);
        if(attempt == 0U) { REQUIRE(host.addDocument(project, document)); }
        REQUIRE(host.bootstrap());
        const auto nextP = host.projectRuntime().catalog(project).value();
        const auto nextD = host.documentRuntime().catalog(project).value();
        REQUIRE(nextP.entries.size() == 1U);
        REQUIRE(nextD.entries.size() == 1U);
        CHECK(nextP.version.epoch != pv.epoch);
        CHECK(nextD.version.epoch != dv.epoch);
        pv = nextP.version;
        dv = nextD.version;
        if(attempt != 0U) {
            REQUIRE(host.projectRuntime().open(project));
            REQUIRE(host.documentRuntime().open(document));
        }
        const auto revisions = host.documents().snapshot(document).value().revisions();
        REQUIRE(host.projectRuntime().close(project));
        CHECK(host.persistence().recover().value().documents.front().revisions == revisions);
        REQUIRE(host.shutdown());
    }
}

TEST_CASE("Lifecycle catalog concurrent snapshots never pair an old version with new state", "[lifecycle-catalog]")
{
    kernel::AppKernel host;
    const auto project = id<kernel::ProjectId>("project.concurrent");
    REQUIRE(host.addProject(project));
    REQUIRE(host.bootstrap());
    std::atomic_bool done{false};
    std::atomic_uint failures{0U};
    std::jthread writer([&] {
        for(unsigned int n = 0U; n < 300U; ++n) {
            if(!host.projectRuntime().close(project) || !host.projectRuntime().open(project)) { ++failures; }
        }
        done.store(true);
    });
    auto previous = host.projectRuntime().catalog(project).value();
    unsigned int reads = 0U;
    do {
        auto current = host.projectRuntime().catalog(project).value();
        if(current.version == previous.version) { CHECK(current.entries.front().state == previous.entries.front().state); }
        CHECK(current.version.epoch == previous.version.epoch);
        CHECK(current.version.revision >= previous.version.revision);
        previous = std::move(current);
        ++reads;
    } while(!done.load() || reads < 100U);
    writer.join();
    CHECK(failures.load() == 0U);
    REQUIRE(host.shutdown());
}

TEST_CASE("Lifecycle catalog exhausted counters never publish wrapped revisions", "[lifecycle-catalog]")
{
    runtime::detail::CatalogCounter counter{std::numeric_limits<std::uint64_t>::max() - 1U, false};
    counter.advance();
    CHECK_FALSE(counter.exhausted);
    REQUIRE(counter.current());
    CHECK(counter.value == std::numeric_limits<std::uint64_t>::max());
    counter.advance();
    CHECK(counter.exhausted);
    CHECK_FALSE(counter.current());
    CHECK(std::string(counter.current().error().code.value()) == "Catalog.RevisionExhausted");
    counter.advance();
    CHECK(counter.value == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("Lifecycle catalog rejected reservations invalidate but untouched rejections do not", "[lifecycle-catalog][fault-matrix]")
{
    for(bool throws : {false, true}) {
        kernel::AppKernel host;
        auto* backend = configure(host, freshRoot());
        const auto project = id<kernel::ProjectId>("project.reservation");
        const auto document = id<kernel::DocumentId>("document.reservation");
        REQUIRE(host.addProject(project));
        REQUIRE(host.bootstrap());
        const auto original = host.documentRuntime().catalog(project).value();
        const auto projectVersion = host.projectRuntime().catalog(project).value().version;
        std::optional<runtime::CatalogVersion> reserved;
        backend->beforeOperation = [&](test::BackendPoint point, std::string_view sql) {
            if(reserved || point != test::BackendPoint::Query || sql.find("document_catalog") == std::string_view::npos) { return; }
            auto snapshot = host.documentRuntime().catalog(project).value();
            REQUIRE(snapshot.entries.size() == 1U);
            CHECK(snapshot.entries.front().state == runtime::DocumentLifecycleState::Opening);
            CHECK(snapshot.version != original.version);
            reserved = snapshot.version;
        };
        backend->arm(test::BackendPoint::Query, "document_catalog", 1U, throws);
        CHECK_FALSE(host.documentRuntime().create(project, document));
        backend->beforeOperation = {};
        REQUIRE(reserved);
        const auto rejected = host.documentRuntime().catalog(project).value();
        CHECK(rejected.entries.empty());
        CHECK(rejected.version != *reserved);
        CHECK(rejected.version != original.version);
        CHECK_FALSE(host.projectRuntime().create(project));
        CHECK(host.projectRuntime().catalog(project).value().version == projectVersion);
        CHECK_FALSE(host.documentRuntime().close(document));
        CHECK_FALSE(host.documentRuntime().remove(document));
        CHECK(host.documentRuntime().catalog(project).value().version == rejected.version);
        REQUIRE(host.documentRuntime().create(project, document));
        REQUIRE(host.documentRuntime().close(document));
        const auto beforeRemove = host.documentRuntime().catalog(project).value().version;
        backend->arm(test::BackendPoint::Execute, "INSERT INTO document_catalog", 1U, throws);
        CHECK_FALSE(host.documentRuntime().remove(document));
        CHECK(host.documentRuntime().catalog(project).value().version == beforeRemove);
        REQUIRE(host.documentRuntime().remove(document));
        CHECK(host.documentRuntime().catalog(project).value().version != beforeRemove);
        REQUIRE(host.shutdown());
    }
}

TEST_CASE("Lifecycle catalog document readers remain coherent during membership churn", "[lifecycle-catalog]")
{
    kernel::AppKernel host;
    const auto project = id<kernel::ProjectId>("project.churn");
    const auto document = id<kernel::DocumentId>("document.churn");
    REQUIRE(host.addDocument(project, document));
    REQUIRE(host.bootstrap());
    std::atomic_bool done{false};
    std::atomic_uint failures{0U};
    std::jthread writer([&] {
        for(unsigned int n = 0U; n < 300U; ++n) {
            if(!host.documentRuntime().close(document) || !host.documentRuntime().remove(document)
               || !host.documentRuntime().create(project, document)) { ++failures; }
        }
        done.store(true);
    });
    auto previous = host.documentRuntime().catalog(project).value();
    unsigned int reads = 0U;
    do {
        auto current = host.documentRuntime().catalog(project).value();
        if(current.version == previous.version) {
            REQUIRE(current.entries.size() == previous.entries.size());
            if(!current.entries.empty()) { CHECK(current.entries.front().state == previous.entries.front().state); }
        }
        CHECK(current.version.revision >= previous.version.revision);
        previous = std::move(current);
        ++reads;
    } while(!done.load() || reads < 100U);
    writer.join();
    CHECK(failures.load() == 0U);
    REQUIRE(host.shutdown());
}
