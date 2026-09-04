#include <lasercnc/infrastructure/filesystem_asset_store.hpp>
#include "snapshot_storage_fixture.hpp"
#include <lasercnc/infrastructure/sha256_hash_service.hpp>
#include <lasercnc/foundation/error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <latch>
#include <limits>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lasercnc::foundation;
using namespace lasercnc::kernel;
using namespace lasercnc::infrastructure;
using namespace lasercnc::state;

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        static std::atomic_ullong sequence{0U};
        path = std::filesystem::temp_directory_path() / ("lasercnc-assets-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
            + '-' + std::to_string(sequence.fetch_add(1U)));
    }
    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
};

std::span<const std::byte> bytes(std::string_view value)
{
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

AssetKind kind(const char* text = "test.binary") { return AssetKind::create(text).value(); }

std::unique_ptr<FilesystemAssetStore> store(const std::filesystem::path& directory, std::size_t limit = 4096U)
{
    auto created = FilesystemAssetStore::create({directory, limit}, std::make_shared<Sha256HashService>());
    if(!created) {
        throw std::logic_error("Unable to create the test asset store");
    }
    return std::move(created).value();
}

std::filesystem::path filePath(const std::filesystem::path& directory, const AssetRef& reference)
{
    return lasercnc::test::snapshotStoragePath(directory, reference.id.value());
}

class InjectedHash final : public lasercnc::platform::IHashService {
public:
    Result<ContentDigest> digest(std::span<const std::byte> content) const override
    {
        if(++calls == failAt) {
            if(throwFailure) {
                throw std::runtime_error("Injected hash exception");
            }
            if(malformed) {
                return ContentDigest::create("sha256:malformed");
            }
            return Result<ContentDigest>::failure(makeError(
                "Test.HashFailure", ErrorCategory::Infrastructure, "Injected hash failure"));
        }
        return actual.digest(content);
    }
    mutable unsigned int calls{0U};
    unsigned int failAt{0U};
    bool throwFailure{false};
    bool malformed{false};
    Sha256HashService actual;
};

} // namespace

TEST_CASE("FilesystemAssetStore publishes verified immutable binary content", "[infrastructure][asset]")
{
    TemporaryDirectory directory;
    auto assets = store(directory.path);
    const std::string content{"a\0b\xff", 4U};
    auto published = assets->publish(kind(), bytes(content));
    REQUIRE(published.hasValue());
    const auto reference = published.value();
    CHECK(reference.byteSize == content.size());
    CHECK(std::string(reference.id.value()).starts_with("asset.sha256."));
    CHECK(reference.digest == Sha256HashService{}.digest(bytes(content)).value());
    auto read = assets->read(reference);
    REQUIRE(read.hasValue());
    CHECK((read.value() == std::vector<std::byte>(bytes(content).begin(), bytes(content).end())));
    REQUIRE(assets->verify(reference).hasValue());
    REQUIRE(assets->publish(kind(), bytes(content)).hasValue());
    CHECK(assets->publish(kind(), bytes(content)).value() == reference);
    auto alternate = assets->publish(kind("test.alternate"), bytes(content));
    REQUIRE(alternate.hasValue());
    CHECK(alternate.value().id != reference.id);
    CHECK(alternate.value().digest == reference.digest);
    assets.reset();
    auto reopened = store(directory.path);
    CHECK((reopened->read(reference).value() == read.value()));
    auto empty = reopened->publish(kind(), {});
    REQUIRE(empty.hasValue());
    CHECK(empty.value().byteSize == 0U);
    CHECK(reopened->read(empty.value()).value().empty());
}

TEST_CASE("FilesystemAssetStore denies forged references missing files and corruption", "[infrastructure][asset]")
{
    TemporaryDirectory directory;
    auto assets = store(directory.path);
    auto published = assets->publish(kind(), bytes("payload"));
    REQUIRE(published.hasValue());
    auto reference = published.value();
    SECTION("identity traversal") { reference.id = AssetId::create("../outside").value(); }
    SECTION("changed kind") { reference.kind = kind("test.forged"); }
    SECTION("changed size") { ++reference.byteSize; }
    SECTION("changed digest") { reference.digest = Sha256HashService{}.digest(bytes("different")).value(); }
    SECTION("missing file") { REQUIRE(std::filesystem::remove(filePath(directory.path, reference))); }
    SECTION("truncated file") {
        std::ofstream output(filePath(directory.path, reference), std::ios::binary | std::ios::trunc);
        output << "bad";
        REQUIRE(output.good());
    }
    SECTION("same-sized content corruption") {
        std::fstream output(filePath(directory.path, reference), std::ios::binary | std::ios::in | std::ios::out);
        output.seekp(-1, std::ios::end);
        output.put('!');
        REQUIRE(output.good());
    }
    CHECK_FALSE(assets->verify(reference).hasValue());
    CHECK_FALSE(assets->read(reference).hasValue());
}

TEST_CASE("FilesystemAssetStore enforces bounds before publication", "[infrastructure][asset]")
{
    TemporaryDirectory directory;
    auto hashes = std::make_shared<Sha256HashService>();
    CHECK_FALSE(FilesystemAssetStore::create({directory.path, 1U}, nullptr).hasValue());
    CHECK_FALSE(FilesystemAssetStore::create({directory.path, 0U}, hashes).hasValue());
    CHECK_FALSE(FilesystemAssetStore::create({directory.path, std::numeric_limits<std::size_t>::max()}, hashes).hasValue());
    auto assets = store(directory.path, 4U);
    CHECK_FALSE(assets->publish(kind(), bytes("12345")).hasValue());
    CHECK_FALSE(assets->publish(AssetKind::create(std::string(257U, 'k')).value(), {}).hasValue());
    CHECK(std::filesystem::is_empty(directory.path));
    CHECK(assets->publish(kind(), bytes("1234")).hasValue());
}

TEST_CASE("FilesystemAssetStore isolates hash failures and permits verified orphan reuse", "[infrastructure][asset][failure]")
{
    TemporaryDirectory directory;
    auto hashes = std::make_shared<InjectedHash>();
    bool orphan = false;
    SECTION("hash error before write") { hashes->failAt = 1U; }
    SECTION("hash exception before write") { hashes->failAt = 1U; hashes->throwFailure = true; }
    SECTION("malformed digest before write") { hashes->failAt = 1U; hashes->malformed = true; }
    SECTION("verification error after atomic write") { hashes->failAt = 4U; orphan = true; }
    auto created = FilesystemAssetStore::create({directory.path, 4096U}, hashes);
    REQUIRE(created.hasValue());
    auto assets = std::move(created).value();
    CHECK_FALSE(assets->publish(kind(), bytes("payload")).hasValue());
    CHECK(std::filesystem::is_empty(directory.path) == !orphan);
    hashes->failAt = 0U;
    auto published = assets->publish(kind(), bytes("payload"));
    REQUIRE(published.hasValue());
    CHECK(assets->verify(published.value()).hasValue());
    CHECK(std::distance(std::filesystem::directory_iterator(directory.path), std::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("FilesystemAssetStore concurrent instances publish one stable asset", "[infrastructure][asset][concurrency]")
{
    TemporaryDirectory directory;
    std::latch ready{8};
    std::vector<std::future<Result<AssetRef>>> writers;
    for(int index = 0; index < 8; ++index) {
        writers.push_back(std::async(std::launch::async, [&, assets = store(directory.path)] {
            ready.arrive_and_wait();
            return assets->publish(kind(), bytes("concurrent immutable payload"));
        }));
    }
    std::optional<AssetRef> expected;
    for(auto& writer : writers) {
        auto result = writer.get();
        REQUIRE(result.hasValue());
        if(!expected.has_value()) {
            expected = result.value();
        }
        CHECK(result.value() == *expected);
    }
    auto assets = store(directory.path);
    REQUIRE(expected.has_value());
    CHECK(assets->verify(*expected).hasValue());
    CHECK(std::distance(std::filesystem::directory_iterator(directory.path), std::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("FilesystemAssetStore publication failure does not replace existing filesystem state", "[infrastructure][asset][failure]")
{
    TemporaryDirectory source;
    const auto reference = store(source.path)->publish(kind(), bytes("payload")).value();
    TemporaryDirectory target;
    auto assets = store(target.path);
    const auto blocker = filePath(target.path, reference);
    REQUIRE(std::filesystem::create_directory(blocker));
    CHECK_FALSE(assets->publish(kind(), bytes("payload")).hasValue());
    CHECK(std::filesystem::is_directory(blocker));
    CHECK(std::filesystem::is_empty(blocker));
}
