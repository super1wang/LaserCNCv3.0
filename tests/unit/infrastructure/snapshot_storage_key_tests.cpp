#include <lasercnc/infrastructure/filesystem_snapshot_store.hpp>
#include <catch2/catch_test_macros.hpp>
#include "snapshot_storage_fixture.hpp"
#include "snapshot_create_probe.hpp"
#include "persistence_fixture.hpp"
#include <lasercnc/infrastructure/filesystem_asset_store.hpp>
#include <lasercnc/state/document_store.hpp>
#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <latch>
#include <limits>
#include <set>
#include <vector>

using namespace lasercnc;
namespace {
std::filesystem::path root()
{
    static std::atomic_uint sequence{0U};
    auto path = std::filesystem::path{LCNC_STRESS_TEST_ROOT} / "snapshot-storage-key"
        / (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-"
            + std::to_string(sequence.fetch_add(1U)));
    std::filesystem::create_directories(path);
    return path;
}
auto store(const std::filesystem::path& directory, std::size_t limit = 1024U)
{
    auto result = infrastructure::FilesystemSnapshotStore::create({directory, limit});
    REQUIRE(result);
    return std::move(result).value();
}
auto id(std::string value) { return kernel::SnapshotId::create(std::move(value)).value(); }
void fixture(const std::filesystem::path& path, std::string_view payload)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    REQUIRE(output.good());
}
}

TEST_CASE("Snapshot storage keys isolate exact byte identities and bound filenames", "[snapshot-storage-key]")
{
    const auto directory = root();
    auto files = store(directory);
    const std::vector<std::string> identities{"snapshot.Case", "snapshot.case", "中文.快照", "../escape",
        "a\\b:c*?\"<>|", "CON", "NUL", "COM1.extension", ".", "..", "snapshot.\xc3\xa9", "snapshot.e\xcc\x81",
        "snapshot.\xf0\x9f\x94\xa7", std::string(128U, 'x'),
        std::string(129U, 'x'), std::string(1024U, 'x'), std::string(4096U, 'x')};
    for(std::size_t index = 0U; index < identities.size(); ++index) {
        DYNAMIC_SECTION("identity " << index) {
            const auto key = id(identities[index]);
            const auto payload = "payload-" + std::to_string(index);
            auto written = files->writeAtomically(key, payload);
            REQUIRE(written);
            CHECK(files->read(key).value() == payload);
            const auto path = test::snapshotStoragePath(directory, key.value());
            CHECK(std::filesystem::is_regular_file(path));
            CHECK(path.filename().string().size() == 74U);
            auto reopened = store(directory);
            CHECK(reopened->read(key).value() == payload);
            REQUIRE(reopened->remove(key));
            CHECK_FALSE(files->remove(key).value());
        }
    }
}

TEST_CASE("Snapshot storage keys preserve case sensitive legacy ownership", "[snapshot-storage-key]")
{
    const auto directory = root();
    auto original = id("Snapshot.Case");
    auto alternate = id("snapshot.case");
    SECTION("uppercase legacy") {}
    SECTION("lowercase legacy") { std::swap(original, alternate); }
    const auto legacy = directory / (std::string(original.value()) + ".snapshot");
    fixture(legacy, "legacy");
    auto files = store(directory);
    REQUIRE(files->read(original));
    CHECK(files->read(original).value() == "legacy");
    REQUIRE_FALSE(files->read(alternate));
    REQUIRE(files->remove(alternate));
    CHECK_FALSE(files->remove(alternate).value());
    CHECK(files->writeAtomically(original, "legacy").value() == platform::SnapshotWriteDisposition::AlreadyPresent);
    REQUIRE_FALSE(files->writeAtomically(original, "changed"));
    REQUIRE(files->writeAtomically(alternate, "new"));
    CHECK(files->read(original).value() == "legacy");
    CHECK(files->read(alternate).value() == "new");
    REQUIRE(files->remove(alternate));
    CHECK(std::filesystem::is_regular_file(legacy));
    REQUIRE(files->remove(original));
}

TEST_CASE("Snapshot storage keys refuse mismatched envelopes and ambiguous formats", "[snapshot-storage-key]")
{
    const auto directory = root();
    const auto key = id("snapshot.envelope");
    auto files = store(directory);
    SECTION("wrong identity at the right hashed path") {
        fixture(test::snapshotStoragePath(directory, key.value()), test::snapshotStorageEnvelope("snapshot.other", "payload"));
    }
    SECTION("truncated length") { fixture(test::snapshotStoragePath(directory, key.value()), "LCNCSN02\xff"); }
    SECTION("empty identity") { fixture(test::snapshotStoragePath(directory, key.value()), test::snapshotStorageEnvelope("", "payload")); }
    SECTION("overlong identity") { fixture(test::snapshotStoragePath(directory, key.value()), test::snapshotStorageEnvelope(std::string(4097U, 'x'), "payload")); }
    SECTION("truncated identity") { fixture(test::snapshotStoragePath(directory, key.value()), test::snapshotStorageEnvelope(key.value(), "payload").substr(0U, 13U)); }
    SECTION("unrecognized format") { fixture(test::snapshotStoragePath(directory, key.value()), "unsupported"); }
    SECTION("two formats must not resurrect on removal") {
        REQUIRE(files->writeAtomically(key, "payload"));
        fixture(directory / "snapshot.envelope.snapshot", "payload");
    }
    const auto fileCount = std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{});
    REQUIRE_FALSE(files->read(key));
    REQUIRE_FALSE(files->writeAtomically(key, "payload"));
    REQUIRE_FALSE(files->remove(key));
    CHECK(std::filesystem::is_regular_file(test::snapshotStoragePath(directory, key.value())));
    CHECK(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{}) == fileCount);
}

TEST_CASE("Snapshot storage keys enforce identity and payload envelope budgets", "[snapshot-storage-key]")
{
    const auto directory = root();
    auto files = store(directory, 4U);
    const auto overlong = kernel::SnapshotId::create(std::string(4097U, 'x'));
    REQUIRE_FALSE(overlong);
    CHECK(std::string(overlong.error().code.value())
          == "Foundation.StrongIdBudgetExceeded");
    CHECK(std::filesystem::is_empty(directory));
    CHECK_FALSE(infrastructure::FilesystemSnapshotStore::create({directory, std::numeric_limits<std::size_t>::max()}));
    const auto key = id("snapshot.budget");
    REQUIRE(files->writeAtomically(key, "1234"));
    CHECK(files->read(key).value() == "1234");
    CHECK_FALSE(files->writeAtomically(id("snapshot.too-big"), "12345"));
    fixture(test::snapshotStoragePath(directory, key.value()), test::snapshotStorageEnvelope(key.value(), "12345"));
    CHECK_FALSE(files->read(key));
}

TEST_CASE("Snapshot storage keys preserve concurrent immutable publication", "[snapshot-storage-key]")
{
    const auto directory = root();
    std::latch ready{8};
    std::vector<std::future<std::string>> writers;
    for(int index = 0; index < 8; ++index) {
        writers.push_back(std::async(std::launch::async, [&, index] {
            auto created = infrastructure::FilesystemSnapshotStore::create({directory, 1024U});
            ready.count_down(); ready.wait();
            if(!created) { return std::string(created.error().code.value()) + ": " + created.error().message; }
            const auto key = id(index % 2 == 0 ? "snapshot.Case" : "snapshot.case");
            const auto payload = index % 2 == 0 ? "upper" : "lower";
            auto result = created.value()->writeAtomically(key, payload);
            if(result) { return std::string{"ok"}; }
            auto details = infrastructure::JsonconsAdapter{}.serialize(result.error().details);
            return std::string(result.error().code.value()) + ": " + (details ? details.value() : result.error().message);
        }));
    }
    for(auto& writer : writers) { CHECK(writer.get() == "ok"); }
    auto files = store(directory);
    CHECK(files->read(id("snapshot.Case")).value() == "upper");
    CHECK(files->read(id("snapshot.case")).value() == "lower");
    CHECK(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{}) == 2);
}

TEST_CASE("Snapshot storage keys retry temporary collisions without touching existing files", "[snapshot-storage-key]")
{
    for(const std::size_t collisions : {2U, 8U}) {
        DYNAMIC_SECTION("collisions " << collisions) {
            const auto directory = root();
            auto files = store(directory);
            std::vector<std::filesystem::path> ownedByOtherWriter;
            struct Reset final { ~Reset() { setSnapshotCreateProbe({}); } } reset;
            setSnapshotCreateProbe([&](const auto& path) {
                if(ownedByOtherWriter.size() < collisions) {
                    REQUIRE_FALSE(std::filesystem::exists(path));
                    fixture(path, "other-writer");
                    ownedByOtherWriter.push_back(path);
                }
            });
            auto written = files->writeAtomically(id("snapshot.retry"), "payload");
            if(collisions == 2U) { REQUIRE(written); }
            else {
                REQUIRE_FALSE(written);
                CHECK(std::string(written.error().code.value()) == "Snapshot.TemporaryCollision");
            }
            setSnapshotCreateProbe({});
            CHECK(ownedByOtherWriter.size() == collisions);
            for(const auto& path : ownedByOtherWriter) {
                std::ifstream input(path, std::ios::binary);
                CHECK(std::string(std::istreambuf_iterator<char>{input}, {}) == "other-writer");
            }
            REQUIRE(files->writeAtomically(id("snapshot.retry"), "payload"));
            CHECK(files->read(id("snapshot.retry")).value() == "payload");
        }
    }
}

TEST_CASE("Snapshot storage keys reject hardlinks directories and mismatched canonical case", "[snapshot-storage-key]")
{
    const auto directory = root();
    auto files = store(directory);
    const auto key = id("snapshot.file-kind");
    const auto target = test::snapshotStoragePath(directory, key.value());
    SECTION("directory") { std::filesystem::create_directory(target); }
    SECTION("hardlink") {
        const auto original = directory / "original";
        fixture(original, test::snapshotStorageEnvelope(key.value(), "payload"));
        std::filesystem::create_hard_link(original, target);
    }
    SECTION("noncanonical hashed filename") {
        auto name = target.filename().string();
        for(auto& character : name) {
            if(character >= 'a' && character <= 'f') { character = static_cast<char>(character - 'a' + 'A'); }
        }
        fixture(directory / name, test::snapshotStorageEnvelope(key.value(), "payload"));
    }
    CHECK_FALSE(files->read(key));
    CHECK_FALSE(files->writeAtomically(key, "payload"));
    CHECK_FALSE(files->remove(key));
    CHECK(std::filesystem::exists(target));
}

TEST_CASE("Snapshot storage keys verify a concurrent winner held by a reader", "[snapshot-storage-key]")
{
    const auto directory = root();
    const auto key = id("snapshot.reader-race");
    auto files = store(directory);
    std::ifstream reader;
    struct Reset final { ~Reset() { setSnapshotPublishProbe({}); } } reset;
    setSnapshotPublishProbe([&](const auto& path) {
        fixture(path, test::snapshotStorageEnvelope(key.value(), "payload"));
        reader.open(path, std::ios::binary);
        REQUIRE(reader.good());
    });
    auto result = files->writeAtomically(key, "payload");
    INFO((result ? "ok" : std::string(result.error().code.value()) + ": "
        + infrastructure::JsonconsAdapter{}.serialize(result.error().details).value()));
    REQUIRE(result);
    CHECK(result.value() == platform::SnapshotWriteDisposition::AlreadyPresent);
    CHECK(files->read(key).value() == "payload");
}

TEST_CASE("Snapshot storage keys support native long directory paths", "[snapshot-storage-key]")
{
    const auto directory = root() / std::string(120U, 'd') / std::string(100U, 'e');
    auto files = store(directory);
    const auto key = id("snapshot.long-path");
    REQUIRE(files->writeAtomically(key, "payload"));
    CHECK(files->read(key).value() == "payload");
    auto reopened = store(directory);
    CHECK(reopened->read(key).value() == "payload");
    REQUIRE(reopened->remove(key));
    CHECK_FALSE(files->read(key));
}

TEST_CASE("Snapshot storage keys reject ambiguous directory normalization before creating files", "[snapshot-storage-key]")
{
    const auto directory = root();
    CHECK_FALSE(infrastructure::FilesystemSnapshotStore::create({directory / "ambiguous.", 1024U}));
    CHECK_FALSE(infrastructure::FilesystemSnapshotStore::create({directory / "ambiguous ", 1024U}));
    CHECK_FALSE(infrastructure::FilesystemSnapshotStore::create({directory / "parent." / "child", 1024U}));
    CHECK(std::filesystem::is_empty(directory));
}

TEST_CASE("Snapshot storage keys bound sharing violation retries without bypassing verification", "[snapshot-storage-key]")
{
    for(const unsigned int failures : {2U, 8U}) {
        DYNAMIC_SECTION("sharing failures " << failures) {
            const auto directory = root();
            auto files = store(directory);
            const auto key = id("snapshot.sharing");
            REQUIRE(files->writeAtomically(key, "payload"));
            struct Reset final { ~Reset() { setSnapshotReadSharingFailures(0U); } } reset;
            setSnapshotReadSharingFailures(failures);
            auto read = files->read(key);
            if(failures == 2U) {
                REQUIRE(read);
                CHECK(read.value() == "payload");
                CHECK(snapshotReadAttempts() == 3U);
            } else {
                REQUIRE_FALSE(read);
                CHECK(std::string(read.error().code.value()) == "Snapshot.ReadFailed");
                CHECK(snapshotReadAttempts() == 8U);
            }
            setSnapshotReadSharingFailures(0U);
            CHECK(files->read(key).value() == "payload");
        }
    }
}

TEST_CASE("Snapshot storage keys recover mixed legacy and encoded persistence without changing ownership", "[snapshot-storage-key]")
{
    const auto directory = root();
    const auto project = kernel::ProjectId::create("project.mixed").value();
    const auto a = kernel::DocumentId::create("document.Case").value();
    const auto b = kernel::DocumentId::create("document.case").value();
    const auto first = id("Snapshot.Legacy");
    const auto second = id("../中文:Snapshot");
    state::DocumentStore documents;
    REQUIRE(documents.addDocument(project, a)); REQUIRE(documents.addDocument(project, b));
    std::string legacyPayload;
    {
        auto persistence = test::openPersistenceFixture(directory / "state.db", directory / "files");
        auto captured = persistence->captureSnapshot(first, documents.snapshot(a).value());
        REQUIRE(captured);
        legacyPayload = captured.value().payload;
        REQUIRE(persistence->captureSnapshot(second, documents.snapshot(b).value()));
    }
    REQUIRE(std::filesystem::remove(test::snapshotStoragePath(directory / "files", first.value())));
    fixture(directory / "files" / "Snapshot.Legacy.snapshot", legacyPayload);
    auto persistence = test::openPersistenceFixture(directory / "state.db", directory / "files");
    CHECK(persistence->latestSnapshot(a).value()->snapshotId == first);
    CHECK(persistence->latestSnapshot(b).value()->snapshotId == second);
    auto restored = persistence->recover();
    REQUIRE(restored);
    REQUIRE(restored.value().documents.size() == 2U);
    for(const auto& document : restored.value().documents) {
        CHECK(document.projectId == project);
        CHECK((document.documentId == a || document.documentId == b));
    }
    REQUIRE(persistence->captureSnapshot(first, documents.snapshot(a).value()));
    CHECK_FALSE(std::filesystem::exists(test::snapshotStoragePath(directory / "files", first.value())));
}

TEST_CASE("Snapshot storage keys retain legacy asset content and reference format", "[snapshot-storage-key]")
{
    const auto directory = root();
    const auto kind = kernel::AssetKind::create("test.legacy").value();
    const std::string content{"payload\0binary", 14U};
    auto assets = infrastructure::FilesystemAssetStore::create({directory, 1024U},
        std::make_shared<infrastructure::Sha256HashService>()).value();
    auto published = assets->publish(kind, std::as_bytes(std::span{content.data(), content.size()}));
    REQUIRE(published);
    const auto reference = published.value();
    const auto key = id(std::string(reference.id.value()));
    auto files = store(directory);
    auto oldAssetEnvelope = files->read(key);
    REQUIRE(oldAssetEnvelope);
    REQUIRE(files->remove(key));
    fixture(directory / (std::string(reference.id.value()) + ".snapshot"), oldAssetEnvelope.value());
    assets.reset();
    auto reopened = infrastructure::FilesystemAssetStore::create({directory, 1024U},
        std::make_shared<infrastructure::Sha256HashService>()).value();
    auto read = reopened->read(reference);
    REQUIRE(read);
    const auto expectedBytes = std::as_bytes(std::span{content.data(), content.size()});
    const std::vector<std::byte> expected(expectedBytes.begin(), expectedBytes.end());
    CHECK((read.value() == expected));
    CHECK(reopened->publish(kind, std::as_bytes(std::span{content.data(), content.size()})).value() == reference);
    CHECK_FALSE(std::filesystem::exists(test::snapshotStoragePath(directory, reference.id.value())));
}
