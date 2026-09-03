#include <lasercnc/infrastructure/filesystem_snapshot_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

using namespace lasercnc::infrastructure;
using namespace lasercnc::kernel;
using namespace lasercnc::platform;

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

std::filesystem::path uniqueDirectory()
{
    static std::atomic_ullong sequence {0U};
    return std::filesystem::temp_directory_path()
        / ("lasercnc-snapshot-store-"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
           + '-' + std::to_string(sequence.fetch_add(1U)));
}

void removeDirectory(const std::filesystem::path& path)
{
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove_all(path, ignored));
}

} // namespace

TEST_CASE("FilesystemSnapshotStore publishes immutable payloads atomically", "[infrastructure][snapshot]")
{
    const auto directory = uniqueDirectory();
    removeDirectory(directory);
    {
        auto created = FilesystemSnapshotStore::create(
            FilesystemSnapshotStoreOptions {directory, 1024U});
        REQUIRE(created.hasValue());
        auto store = std::move(created).value();
        const auto id = validId<SnapshotId>("snapshot.atomic-1");

        auto first = store->writeAtomically(id, "payload");
        REQUIRE(first.hasValue());
        CHECK(first.value() == SnapshotWriteDisposition::Created);
        REQUIRE(store->read(id).hasValue());
        CHECK(store->read(id).value() == "payload");

        auto repeated = store->writeAtomically(id, "payload");
        REQUIRE(repeated.hasValue());
        CHECK(repeated.value() == SnapshotWriteDisposition::AlreadyPresent);
        auto conflict = store->writeAtomically(id, "different");
        REQUIRE_FALSE(conflict.hasValue());
        CHECK(std::string(conflict.error().code.value())
              == "Snapshot.IdentityConflict");

        auto removed = store->remove(id);
        REQUIRE(removed.hasValue());
        CHECK(removed.value());
        CHECK_FALSE(store->remove(id).value());
        auto missing = store->read(id);
        REQUIRE_FALSE(missing.hasValue());
        CHECK(std::string(missing.error().code.value()) == "Snapshot.NotFound");
    }
    removeDirectory(directory);
}

TEST_CASE("FilesystemSnapshotStore rejects traversal and oversized payloads", "[infrastructure][snapshot]")
{
    CHECK_FALSE(FilesystemSnapshotStore::create({{}, 1U}).hasValue());
    const auto directory = uniqueDirectory();
    removeDirectory(directory);
    {
        auto created = FilesystemSnapshotStore::create(
            FilesystemSnapshotStoreOptions {directory, 4U});
        REQUIRE(created.hasValue());
        auto store = std::move(created).value();
        auto traversal = store->writeAtomically(
            validId<SnapshotId>("../escape"), "safe");
        REQUIRE_FALSE(traversal.hasValue());
        CHECK(std::string(traversal.error().code.value())
              == "Snapshot.InvalidIdForStore");
        auto oversized = store->writeAtomically(
            validId<SnapshotId>("snapshot.large"), "12345");
        REQUIRE_FALSE(oversized.hasValue());
        CHECK(std::string(oversized.error().code.value())
              == "Snapshot.PayloadTooLarge");
        CHECK(std::filesystem::is_empty(directory));
    }
    removeDirectory(directory);
}
