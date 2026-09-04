#include <lasercnc/kernel/app_kernel.hpp>
#include <catch2/catch_test_macros.hpp>
#include "persistence_fixture.hpp"
#include "../../../src/runtime/document/close_snapshot_identity.hpp"
#include <atomic>
#include <chrono>
#include <set>
#include <string>
#include <vector>

using namespace lasercnc;
namespace {
std::filesystem::path freshRoot()
{
    static std::atomic_uint sequence{0U};
    auto root = std::filesystem::path{LCNC_STRESS_TEST_ROOT} / "close-snapshot-identity"
        / (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-"
           + std::to_string(sequence.fetch_add(1U)));
    std::filesystem::create_directories(root);
    return root;
}
void configure(kernel::AppKernel& host, const std::filesystem::path& root)
{
    auto backend = infrastructure::SqlitePersistenceBackend::open({root / "state.db"});
    auto snapshots = infrastructure::FilesystemSnapshotStore::create({root / "snapshots", 1024U * 1024U});
    REQUIRE(backend);
    REQUIRE(snapshots);
    REQUIRE(host.configurePersistence(std::move(backend).value(),
        std::make_shared<infrastructure::JsonconsAdapter>(),
        std::make_shared<infrastructure::Sha256HashService>(), std::move(snapshots).value()));
}
}

TEST_CASE("Close snapshot identity accepts document byte identities without filename coupling", "[close-snapshot-identity]")
{
    const std::vector<std::string> values{
        "document.中文", "document.Case", "document.case", "../outside", "a\\b:c*?\"<>|",
        "CON", "NUL", ".", "..", std::string(96U, 'a'), std::string(128U, 'b'),
        std::string(129U, 'c'), std::string(1024U, 'd')};
    for(std::size_t index = 0U; index < values.size(); ++index) {
        DYNAMIC_SECTION("identity fixture " << index) {
            const auto root = freshRoot();
            const auto project = kernel::ProjectId::create("project.snapshot-identity").value();
            const auto document = kernel::DocumentId::create(values[index]).value();
            std::set<std::string> generated;
            for(int restart = 0; restart < 2; ++restart) {
                kernel::AppKernel host;
                configure(host, root);
                if(restart == 0) { REQUIRE(host.addDocument(project, document)); }
                REQUIRE(host.bootstrap());
                if(restart != 0) {
                    CHECK_FALSE(host.documents().contains(document));
                    REQUIRE(host.documentRuntime().open(document));
                }
                for(int close = 0; close < 2; ++close) {
                    auto closed = host.documentRuntime().close(document);
                    INFO((closed ? "closed" : std::string(closed.error().code.value()) + ": " + closed.error().message));
                    REQUIRE(closed);
                    auto latest = host.persistence().latestSnapshot(document);
                    REQUIRE(latest);
                    REQUIRE(latest.value());
                    CHECK(latest.value()->documentId == document);
                    CHECK(latest.value()->projectId == project);
                    // Count immutable keys, not created_at ordering (audited separately in C3c).
                    // 中文翻译：计算不可变键，不将 created_at 排序等同于插入顺序；排序另由 C3c 审计。
                    for(const auto& entry : std::filesystem::directory_iterator(root / "snapshots")) {
                        generated.insert(entry.path().filename().string());
                    }
                    CHECK(generated.size() == static_cast<std::size_t>(restart * 3 + close + 1));
                    REQUIRE(host.documentRuntime().open(document));
                    auto restored = host.documents().snapshot(document);
                    REQUIRE(restored);
                    CHECK(restored.value().id() == document);
                }
                REQUIRE(host.documentRuntime().close(document));
                REQUIRE(host.shutdown());
            }
            for(const auto& entry : std::filesystem::directory_iterator(root / "snapshots")) {
                CHECK(entry.path().extension() == ".snapshot");
                CHECK(entry.path().filename().string().size() <= 128U);
            }
        }
    }
}

TEST_CASE("Close snapshot identity is opaque fixed lowercase ASCII", "[close-snapshot-identity]")
{
    const auto root = freshRoot();
    const auto project = kernel::ProjectId::create("project.opaque").value();
    const auto document = kernel::DocumentId::create("document.must-not-appear-in-storage-key").value();
    kernel::AppKernel host;
    configure(host, root);
    REQUIRE(host.addDocument(project, document));
    REQUIRE(host.bootstrap());
    REQUIRE(host.documentRuntime().close(document));
    auto latest = host.persistence().latestSnapshot(document);
    REQUIRE(latest);
    REQUIRE(latest.value());
    const auto value = latest.value()->snapshotId.value();
    CHECK(value.starts_with("snapshot.close.v2."));
    CHECK(value.size() == 82U);
    CHECK(value.find(document.value()) == std::string_view::npos);
    CHECK(value.substr(18U).find_first_not_of("0123456789abcdef") == std::string_view::npos);
    REQUIRE(host.shutdown());
}

TEST_CASE("Close snapshot identity private generator handles entropy failure without clock fallback", "[close-snapshot-identity]")
{
    auto fixed = runtime::detail::closeSnapshotIdentity([](auto& words) { words.fill(0x0123abcdU); });
    REQUIRE(fixed);
    CHECK(std::string(fixed.value().value()) ==
        "snapshot.close.v2.0123abcd0123abcd0123abcd0123abcd0123abcd0123abcd0123abcd0123abcd");
    auto zero = runtime::detail::closeSnapshotIdentity([](auto&) {});
    auto standard = runtime::detail::closeSnapshotIdentity([](auto&) { throw std::runtime_error("entropy failed"); });
    auto unknown = runtime::detail::closeSnapshotIdentity([](auto&) { throw 7; });
    for(const auto* failure : {&zero, &standard, &unknown}) {
        REQUIRE_FALSE(*failure);
        CHECK(std::string(failure->error().code.value()) == "Snapshot.IdentityGenerationFailed");
    }
}

TEST_CASE("Close snapshot identity collision cannot overwrite another document", "[close-snapshot-identity]")
{
    const auto root = freshRoot();
    const auto project = kernel::ProjectId::create("project.collision").value();
    const auto a = kernel::DocumentId::create("document.Case").value();
    const auto b = kernel::DocumentId::create("document.case").value();
    state::DocumentStore documents;
    REQUIRE(documents.addDocument(project, a));
    REQUIRE(documents.addDocument(project, b));
    auto service = test::openPersistenceFixture(root / "state.db", root / "snapshots");
    const auto firstId = runtime::detail::closeSnapshotIdentity([](auto& words) { words.fill(1U); }).value();
    const auto collidingId = runtime::detail::closeSnapshotIdentity([](auto& words) { words.fill(1U); }).value();
    auto first = service->captureSnapshot(firstId, documents.snapshot(a).value());
    REQUIRE(first);
    auto conflict = service->captureSnapshot(collidingId, documents.snapshot(b).value());
    REQUIRE_FALSE(conflict);
    CHECK(std::string(conflict.error().code.value()) == "Persistence.SnapshotIdentityConflict");
    auto retained = service->latestSnapshot(a);
    REQUIRE(retained);
    REQUIRE(retained.value());
    CHECK(retained.value()->payload == first.value().payload);
    REQUIRE(service->latestSnapshot(b));
    CHECK_FALSE(service->latestSnapshot(b).value());
    CHECK(service->captureSnapshot(firstId, documents.snapshot(a).value()));
}
