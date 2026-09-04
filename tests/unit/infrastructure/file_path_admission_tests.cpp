#include <lasercnc/infrastructure/filesystem_asset_store.hpp>
#include <lasercnc/infrastructure/filesystem_snapshot_store.hpp>
#include <lasercnc/infrastructure/sha256_hash_service.hpp>
#include <lasercnc/infrastructure/spdlog_log_service.hpp>
#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace {
using namespace lasercnc::foundation;
using namespace lasercnc::infrastructure;
using namespace lasercnc::kernel;

std::filesystem::path root()
{
    static std::atomic_uint sequence{0U};
    auto path = std::filesystem::path{LCNC_STRESS_TEST_ROOT} / "file-path-admission"
        / (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
            + "-" + std::to_string(sequence.fetch_add(1U)));
    REQUIRE(std::filesystem::create_directories(path));
    return path;
}
std::filesystem::path withNull(const std::filesystem::path& prefix, bool suffix)
{
    auto native = prefix.native();
    native.push_back(std::filesystem::path::value_type{});
    if(suffix) { native.append(L"-suffix"); }
    return std::filesystem::path{native};
}
template<typename T> Result<void> discard(Result<T> result)
{
    if(!result) { return Result<void>::failure(std::move(result).error()); }
    return Result<void>::success();
}
Result<void> admit(unsigned int adapter, const std::filesystem::path& path)
{
    if(adapter == 0U) { return discard(SqlitePersistenceBackend::open({path})); }
    if(adapter == 1U) { return discard(FilesystemSnapshotStore::create({path, 4096U})); }
    if(adapter == 2U) { return discard(FilesystemAssetStore::create({path, 4096U}, std::make_shared<Sha256HashService>())); }
    SpdlogLogOptions options;
    options.enableConsole = false;
    if(adapter == 3U) { options.rotatingFilePath = path; }
    else { options.jsonlFilePath = path; }
    return discard(SpdlogLogService::create(options));
}
std::string read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}
} // namespace

TEST_CASE("File adapters reject embedded NUL paths before creating prefix state", "[infrastructure][path][admission][c6]")
{
    for(unsigned int adapter = 0U; adapter < 5U; ++adapter) {
        for(const bool suffix : {false, true}) {
            DYNAMIC_SECTION("adapter=" << adapter << " suffix=" << suffix) {
                const auto directory = root();
                const auto prefix = directory / "target";
                const auto invalid = withNull(prefix, suffix);
                INFO("evidence=" << directory.string());
                const auto admitted = admit(adapter, invalid);
                CHECK_FALSE(admitted);
                if(!admitted) {
                    const std::array expected{"Persistence.InvalidOptions", "Snapshot.InvalidStoreOptions",
                        "Asset.StoreInitializationFailed", "Logging.InvalidOptions", "Logging.InvalidOptions"};
                    CHECK(std::string(admitted.error().code.value()) == expected[adapter]);
                    if(adapter == 2U) {
                        REQUIRE(admitted.error().cause);
                        CHECK(std::string(admitted.error().cause->code.value()) == "Snapshot.InvalidStoreOptions");
                    }
                }
                CHECK_FALSE(std::filesystem::exists(prefix));
                CHECK(std::filesystem::is_empty(directory));
            }
        }
    }
}

TEST_CASE("File path rejection preserves existing prefix files and directories", "[infrastructure][path][admission][c6]")
{
    for(unsigned int adapter = 0U; adapter < 5U; ++adapter) {
        for(const bool suffix : {false, true}) {
            DYNAMIC_SECTION("adapter=" << adapter << " suffix=" << suffix) {
                const auto directory = root();
                const auto prefix = directory / "existing";
                const bool folder = adapter == 1U || adapter == 2U;
                if(folder) { REQUIRE(std::filesystem::create_directory(prefix)); }
                const auto marker = folder ? prefix / "sentinel" : prefix;
                {
                    std::ofstream file(marker, std::ios::binary);
                    file << "preserve existing bytes";
                    REQUIRE(file.good());
                }
                INFO("evidence=" << directory.string());
                CHECK_FALSE(admit(adapter, withNull(prefix, suffix)));
                CHECK(read(marker) == "preserve existing bytes");
                CHECK(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{}) == 1);
            }
        }
    }
}

TEST_CASE("Logging path admission validates both sinks before creating either output", "[logging][path][admission][c6]")
{
    for(const bool humanInvalid : {false, true}) {
        DYNAMIC_SECTION("humanInvalid=" << humanInvalid) {
            const auto directory = root();
            INFO("evidence=" << directory.string());
            SpdlogLogOptions options;
            options.enableConsole = false;
            options.rotatingFilePath = humanInvalid ? withNull(directory / "human.log", true) : directory / "human.log";
            options.jsonlFilePath = humanInvalid ? directory / "events.jsonl" : withNull(directory / "events.jsonl", true);
            const auto created = SpdlogLogService::create(options);
            CHECK_FALSE(created);
            if(!created) { CHECK(std::string(created.error().code.value()) == "Logging.InvalidOptions"); }
            CHECK(std::filesystem::is_empty(directory));
        }
    }
}

TEST_CASE("File adapters preserve valid Unicode paths and perform real round trips", "[infrastructure][path][unicode][c6]")
{
    const auto directory = root();
    INFO("evidence=" << directory.string());
    const auto database = directory / std::filesystem::path{u8"数据库-🔧.db"};
    {
        auto opened = SqlitePersistenceBackend::open({database});
        REQUIRE(opened);
        REQUIRE(opened.value()->acquireHostSession());
        REQUIRE(opened.value()->execute("CREATE TABLE proof(value INTEGER)"));
        REQUIRE(opened.value()->execute("INSERT INTO proof VALUES(7)"));
    }
    {
        auto opened = SqlitePersistenceBackend::open({database});
        REQUIRE(opened);
        REQUIRE(opened.value()->acquireHostSession());
        auto values = opened.value()->query("SELECT value FROM proof");
        REQUIRE(values);
        REQUIRE(values.value().size() == 1U);
        CHECK(values.value().front().at("value") == Value{std::int64_t{7}});
    }
    CHECK(std::filesystem::exists(database));
    const auto snapshots = directory / std::filesystem::path{u8"快照-🔧"};
    const auto identity = SnapshotId::create("snapshot.unicode-path").value();
    {
        auto store = FilesystemSnapshotStore::create({snapshots, 4096U});
        REQUIRE(store);
        REQUIRE(store.value()->writeAtomically(identity, "snapshot content"));
    }
    {
        auto store = FilesystemSnapshotStore::create({snapshots, 4096U});
        REQUIRE(store);
        REQUIRE(store.value()->read(identity));
        CHECK(store.value()->read(identity).value() == "snapshot content");
    }
    const auto assets = directory / std::filesystem::path{u8"资产-🔧"};
    auto assetStore = FilesystemAssetStore::create({assets, 4096U}, std::make_shared<Sha256HashService>());
    REQUIRE(assetStore);
    const std::string payload = "asset content";
    auto asset = assetStore.value()->publish(AssetKind::create("test.binary").value(),
        std::as_bytes(std::span{payload.data(), payload.size()}));
    REQUIRE(asset);
    REQUIRE(assetStore.value()->verify(asset.value()));
    const auto human = directory / std::filesystem::path{u8"人类日志-🔧.log"};
    const auto jsonl = directory / std::filesystem::path{u8"结构日志-🔧.jsonl"};
    {
        SpdlogLogOptions options;
        options.enableConsole = false;
        options.rotatingFilePath = human;
        options.jsonlFilePath = jsonl;
        options.rotatingFileMaxBytes = 128U;
        auto logger = SpdlogLogService::create(options);
        REQUIRE(logger);
        lasercnc::observability::LogRecord record;
        record.module = "kernel.path";
        record.category = "admission";
        record.message = "unicode path round trip";
        REQUIRE(logger.value()->write(record));
        REQUIRE(logger.value()->write(record));
        REQUIRE(logger.value()->flush());
    }
    CHECK(read(human).find("unicode path round trip") != std::string::npos);
    CHECK(read(jsonl).find("unicode path round trip") != std::string::npos);
    CHECK(read(human.parent_path() / (human.stem().native() + L".1" + human.extension().native()))
        .find("unicode path round trip") != std::string::npos);
    CHECK(read(jsonl.parent_path() / (jsonl.stem().native() + L".1" + jsonl.extension().native()))
        .find("unicode path round trip") != std::string::npos);
}
