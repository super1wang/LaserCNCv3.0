#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>

#include <catch2/catch_test_macros.hpp>
#include "fault_injecting_backend.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <latch>
#include <string>
#include <vector>

using namespace lasercnc::foundation;
using lasercnc::infrastructure::SqliteConnectionOptions;
using lasercnc::infrastructure::SqlitePersistenceBackend;

namespace {

std::unique_ptr<SqlitePersistenceBackend> openMemoryDatabase()
{
    auto opened = SqlitePersistenceBackend::open(SqliteConnectionOptions {":memory:"});
    REQUIRE(opened.hasValue());
    return std::move(opened).value();
}

std::filesystem::path uniqueDatabasePath()
{
    static std::atomic<unsigned long long> sequence {0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
           / ("lasercnc-sqlite-" + std::to_string(tick) + '-'
              + std::to_string(sequence.fetch_add(1)) + ".db");
}

void removeFile(const std::filesystem::path& path)
{
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove(path, ignored));
}

} // namespace

TEST_CASE("SQLite Host session excludes aliases until backend destruction across threads", "[sqlite][host-session]")
{
    const auto root = std::filesystem::path{LCNC_STRESS_TEST_ROOT} / "host-session"
        / uniqueDatabasePath().stem();
    std::filesystem::create_directories(root);
    const auto path = root / "state.db";
    auto alias = path;
    bool hardLink = false;
    SECTION("same path") {}
    SECTION("dot path alias") { alias = root / "." / "state.db"; }
    SECTION("case alias") { alias = root / "STATE.DB"; }
    SECTION("hard link alias") { alias = root / "alias.db"; hardLink = true; }
    INFO("Retained Host fixture: " << root.string());
    auto opened = SqlitePersistenceBackend::open({path});
    REQUIRE(opened);
    auto first = std::move(opened).value();
    auto admitted = first->acquireHostSession();
    REQUIRE(admitted);
    CHECK(admitted.value().persistent);
    CHECK(admitted.value().backend == "sqlite");
    CHECK(std::filesystem::file_size(path) == 0U);
    REQUIRE(first->beginTransaction());
    REQUIRE(first->execute("CREATE TABLE proof(value INTEGER)"));
    REQUIRE(first->execute("INSERT INTO proof VALUES(7)"));
    REQUIRE(first->commitTransaction());
    if(hardLink) { std::filesystem::create_hard_link(path, alias); }
    auto second = SqlitePersistenceBackend::open({alias});
    REQUIRE(second);
    auto denied = second.value()->acquireHostSession();
    REQUIRE_FALSE(denied);
    CHECK(std::string(denied.error().code.value()) == "Persistence.HostAlreadyOwned");
    auto read = second.value()->query("SELECT value FROM proof");
    REQUIRE(read);
    CHECK(read.value().front().at("value") == Value{std::int64_t{7}});
    REQUIRE(first->beginTransaction());
    REQUIRE(first->execute("INSERT INTO proof VALUES(8)"));
    REQUIRE(first->rollbackTransaction());
    REQUIRE(first->execute("VACUUM"));
    CHECK_FALSE(second.value()->acquireHostSession());
    auto release = std::async(std::launch::async, [owned = std::move(first)]() mutable {
        const bool readmitted = owned->acquireHostSession().hasValue();
        owned.reset();
        return readmitted;
    });
    REQUIRE(release.get());
    REQUIRE(second.value()->acquireHostSession());
    REQUIRE(second.value()->beginTransaction());
    REQUIRE(second.value()->execute("INSERT INTO proof VALUES(9)"));
    REQUIRE(second.value()->commitTransaction());
    CHECK(second.value()->query("SELECT value FROM proof").value().size() == 2U);
}

TEST_CASE("SQLite Host session admits exactly one simultaneous contender", "[sqlite][host-session][concurrency]")
{
    for(unsigned int round = 0U; round < 20U; ++round) {
        const auto root = std::filesystem::path{LCNC_STRESS_TEST_ROOT} / "host-session" / uniqueDatabasePath().stem();
        std::filesystem::create_directories(root);
        auto a = SqlitePersistenceBackend::open({root / "race.db"});
        auto b = SqlitePersistenceBackend::open({root / "race.db"});
        REQUIRE(a);
        REQUIRE(b);
        std::latch start{1};
        auto one = std::async(std::launch::async, [&] { start.wait(); return a.value()->acquireHostSession(); });
        auto two = std::async(std::launch::async, [&] { start.wait(); return b.value()->acquireHostSession(); });
        start.count_down();
        auto first = one.get();
        auto second = two.get();
        REQUIRE(first.hasValue() != second.hasValue());
        CHECK(std::string((first ? second : first).error().code.value()) == "Persistence.HostAlreadyOwned");
    }
}

TEST_CASE("SQLite Host session fault proxy refuses or forwards real ownership", "[sqlite][host-session][fault-matrix]")
{
    for(const bool throws : {false, true}) {
        const auto root = std::filesystem::path{LCNC_STRESS_TEST_ROOT} / "host-session" / uniqueDatabasePath().stem();
        std::filesystem::create_directories(root);
        auto opened = SqlitePersistenceBackend::open({root / "proxy.db"});
        REQUIRE(opened);
        lasercnc::test::FaultInjectingBackend proxy{std::move(opened).value()};
        proxy.arm(lasercnc::test::BackendPoint::HostSession, "", 1U, throws);
        if(throws) { CHECK_THROWS_AS(proxy.acquireHostSession(), std::runtime_error); }
        else { CHECK_FALSE(proxy.acquireHostSession()); }
        auto other = SqlitePersistenceBackend::open({root / "proxy.db"});
        REQUIRE(other);
        REQUIRE(other.value()->acquireHostSession());
        auto denied = proxy.acquireHostSession();
        REQUIRE_FALSE(denied);
        CHECK(std::string(denied.error().code.value()) == "Persistence.HostAlreadyOwned");
        other.value().reset();
        REQUIRE(proxy.acquireHostSession());
    }
}

TEST_CASE("SQLite Host session sets and reads back explicit file durability policy", "[sqlite][host-session]")
{
    const auto root = std::filesystem::path{LCNC_STRESS_TEST_ROOT} / "host-session" / uniqueDatabasePath().stem();
    std::filesystem::create_directories(root);
    auto opened = SqlitePersistenceBackend::open({root / "policy.db", 5000, false});
    REQUIRE(opened);
    auto& database = *opened.value();
    REQUIRE(database.query("PRAGMA journal_mode=MEMORY"));
    REQUIRE(database.execute("PRAGMA synchronous=OFF"));
    auto admitted = database.acquireHostSession();
    REQUIRE(admitted);
    REQUIRE(admitted.value().configuration.getIf<Value::Object>() != nullptr);
    const auto& info = *admitted.value().configuration.getIf<Value::Object>();
    CHECK(info.at("journalMode") == Value{"delete"});
    CHECK(info.at("synchronous") == Value{std::int64_t{3}});
    CHECK(info.at("foreignKeys") == Value{std::int64_t{1}});
    CHECK(info.at("ownership") == Value{"native-file-range-lock-v1"});
    CHECK(database.query("PRAGMA journal_mode").value().front().at("journal_mode") == info.at("journalMode"));
    CHECK(database.query("PRAGMA synchronous").value().front().at("synchronous") == info.at("synchronous"));
    CHECK(database.query("PRAGMA foreign_keys").value().front().at("foreign_keys") == info.at("foreignKeys"));
    auto other = SqlitePersistenceBackend::open({root / "other.db"});
    REQUIRE(other);
    REQUIRE(other.value()->acquireHostSession());
}

TEST_CASE("SQLite Host session rejects policy drift on repeated admission and transactions", "[sqlite][host-session]")
{
    const auto root = std::filesystem::path{LCNC_STRESS_TEST_ROOT} / "host-session" / uniqueDatabasePath().stem();
    std::filesystem::create_directories(root);
    auto opened = SqlitePersistenceBackend::open({root / "drift.db"});
    REQUIRE(opened);
    auto database = std::move(opened).value();
    std::string alter = "PRAGMA synchronous=OFF";
    std::string restore = "PRAGMA synchronous=EXTRA";
    std::string read = "PRAGMA synchronous";
    std::string column = "synchronous";
    Value changed{std::int64_t{0}};
    bool row = false;
    SECTION("synchronous drift") {}
    SECTION("foreign key drift") { alter = "PRAGMA foreign_keys=OFF"; restore = "PRAGMA foreign_keys=ON";
        read = "PRAGMA foreign_keys"; column = "foreign_keys"; }
    SECTION("journal mode drift") { alter = "PRAGMA journal_mode=MEMORY"; restore = "PRAGMA journal_mode=DELETE";
        read = "PRAGMA journal_mode"; column = "journal_mode"; changed = Value{"memory"}; row = true; }
    REQUIRE(database->acquireHostSession());
    if(row) { REQUIRE(database->query(alter)); } else { REQUIRE(database->execute(alter)); }
    auto repeated = database->acquireHostSession();
    CHECK_FALSE(repeated);
    auto begun = database->beginTransaction();
    CHECK_FALSE(begun);
    if(begun) { REQUIRE(database->rollbackTransaction()); }
    CHECK(database->query(read).value().front().at(column) == changed);
    auto competitor = SqlitePersistenceBackend::open({root / "drift.db"});
    REQUIRE(competitor);
    CHECK_FALSE(competitor.value()->acquireHostSession());
    if(row) { REQUIRE(database->query(restore)); } else { REQUIRE(database->execute(restore)); }
    REQUIRE(database->acquireHostSession());
    REQUIRE(database->beginTransaction());
    REQUIRE(database->rollbackTransaction());
}

TEST_CASE("SQLite Host session identifies private memory without claiming file durability", "[sqlite][host-session]")
{
    auto first = openMemoryDatabase();
    auto second = openMemoryDatabase();
    auto admitted = first->acquireHostSession();
    REQUIRE(admitted);
    CHECK_FALSE(admitted.value().persistent);
    const auto& info = *admitted.value().configuration.getIf<Value::Object>();
    CHECK(info.at("journalMode") == Value{"memory"});
    CHECK(info.at("ownership") == Value{"private-memory"});
    REQUIRE(second->acquireHostSession());
}

TEST_CASE("SQLite Host session refuses admission inside a transaction", "[sqlite][host-session]")
{
    auto database = openMemoryDatabase();
    REQUIRE(database->beginTransaction());
    auto admitted = database->acquireHostSession();
    REQUIRE_FALSE(admitted);
    CHECK(std::string(admitted.error().code.value()) == "Persistence.TransactionStateConflict");
    REQUIRE(database->rollbackTransaction());
    REQUIRE(database->acquireHostSession());
}

TEST_CASE("SqlitePersistenceBackend executes parameterized control-plane SQL", "[infrastructure][sqlite]")
{
    auto database = openMemoryDatabase();
    REQUIRE(database->execute(
        "CREATE TABLE metadata(id INTEGER PRIMARY KEY, enabled INTEGER, ratio REAL, name TEXT, note TEXT)")
                .hasValue());

    const std::array parameters {
        Value {std::int64_t {7}}, Value {true}, Value {2.5}, Value {"demo"}, Value {}};
    auto inserted = database->execute(
        "INSERT INTO metadata(id, enabled, ratio, name, note) VALUES(?, ?, ?, ?, ?)",
        parameters);
    REQUIRE(inserted.hasValue());
    CHECK(inserted.value() == 1U);
    auto createdAfterMutation = database->execute("CREATE TABLE empty_control_plane(value INTEGER)");
    REQUIRE(createdAfterMutation.hasValue());
    CHECK(createdAfterMutation.value() == 0U);

    auto returning = database->execute("INSERT INTO empty_control_plane(value) VALUES(1) RETURNING value");
    REQUIRE_FALSE(returning.hasValue());
    CHECK(std::string(returning.error().code.value()) == "Persistence.UnexpectedRows");
    auto emptyControlPlane = database->query("SELECT value FROM empty_control_plane");
    REQUIRE(emptyControlPlane.hasValue());
    CHECK(emptyControlPlane.value().empty());

    const std::array queryParameters {Value {std::int64_t {7}}};
    auto queried = database->query(
        "SELECT id, enabled, ratio, name, note FROM metadata WHERE id = ?", queryParameters);
    REQUIRE(queried.hasValue());
    REQUIRE(queried.value().size() == 1U);
    const auto& row = queried.value().front();
    CHECK(*row.at("id").getIf<std::int64_t>() == 7);
    CHECK(*row.at("enabled").getIf<std::int64_t>() == 1);
    CHECK(*row.at("ratio").getIf<double>() == 2.5);
    CHECK(*row.at("name").getIf<std::string>() == "demo");
    CHECK(row.at("note").kind() == Value::Kind::Null);
}

TEST_CASE("SqlitePersistenceBackend owns low-level transaction state", "[infrastructure][sqlite]")
{
    auto database = openMemoryDatabase();
    REQUIRE(database->execute("CREATE TABLE journal(value INTEGER)").hasValue());
    auto foreignKeys = database->query("PRAGMA foreign_keys");
    REQUIRE(foreignKeys.hasValue());
    REQUIRE(foreignKeys.value().size() == 1U);
    CHECK(*foreignKeys.value().front().at("foreign_keys").getIf<std::int64_t>() == 1);
    REQUIRE(database->beginTransaction().hasValue());

    auto duplicateBegin = database->beginTransaction();
    REQUIRE_FALSE(duplicateBegin.hasValue());
    CHECK(std::string(duplicateBegin.error().code.value())
          == "Persistence.TransactionStateConflict");

    const std::array parameter {Value {std::int64_t {9}}};
    REQUIRE(database->execute("INSERT INTO journal(value) VALUES(?)", parameter).hasValue());
    REQUIRE(database->rollbackTransaction().hasValue());

    auto rows = database->query("SELECT value FROM journal");
    REQUIRE(rows.hasValue());
    CHECK(rows.value().empty());

    auto duplicateRollback = database->rollbackTransaction();
    REQUIRE_FALSE(duplicateRollback.hasValue());
    CHECK(std::string(duplicateRollback.error().code.value())
          == "Persistence.TransactionStateConflict");

    REQUIRE(database->beginTransaction().hasValue());
    REQUIRE(database->execute("INSERT INTO journal(value) VALUES(?)", parameter).hasValue());
    REQUIRE(database->commitTransaction().hasValue());
    rows = database->query("SELECT value FROM journal");
    REQUIRE(rows.hasValue());
    CHECK(rows.value().size() == 1U);
}

TEST_CASE("SqlitePersistenceBackend rejects ambiguous and unsupported values", "[infrastructure][sqlite]")
{
    auto database = openMemoryDatabase();
    REQUIRE(database->execute("CREATE TABLE mutation_guard(value INTEGER)").hasValue());

    auto sqlFailure = database->execute("CREATE TABL broken(value INTEGER)");
    REQUIRE_FALSE(sqlFailure.hasValue());
    CHECK(std::string(sqlFailure.error().code.value()) == "Persistence.DatabaseFailed");

    const std::array wrongCount {Value {std::int64_t {1}}};
    auto mismatch = database->query("SELECT ?, ?", wrongCount);
    REQUIRE_FALSE(mismatch.hasValue());
    CHECK(std::string(mismatch.error().code.value()) == "Persistence.ParameterCountMismatch");

    const std::array compound {Value {Value::Array {Value {std::int64_t {1}}}}};
    auto unsupported = database->query("SELECT ?", compound);
    REQUIRE_FALSE(unsupported.hasValue());
    CHECK(std::string(unsupported.error().code.value()) == "Persistence.UnsupportedParameter");

    auto multiple = database->query("SELECT 1; SELECT 2");
    REQUIRE_FALSE(multiple.hasValue());
    CHECK(std::string(multiple.error().code.value()) == "Persistence.MultipleStatementsDenied");

    auto blob = database->query("SELECT X'CAFE' AS payload");
    REQUIRE_FALSE(blob.hasValue());
    CHECK(std::string(blob.error().code.value()) == "Persistence.UnsupportedColumnType");

    auto duplicate = database->query("SELECT 1 AS value, 2 AS value");
    REQUIRE_FALSE(duplicate.hasValue());
    CHECK(std::string(duplicate.error().code.value()) == "Persistence.DuplicateColumnName");

    auto wrongOperation = database->query("INSERT INTO mutation_guard(value) VALUES(1)");
    REQUIRE_FALSE(wrongOperation.hasValue());
    CHECK(std::string(wrongOperation.error().code.value()) == "Persistence.NoResultColumns");
    auto guardedRows = database->query("SELECT value FROM mutation_guard");
    REQUIRE(guardedRows.hasValue());
    CHECK(guardedRows.value().empty());
}

TEST_CASE("SqlitePersistenceBackend persists metadata to a file", "[infrastructure][sqlite]")
{
    const auto path = uniqueDatabasePath();
    removeFile(path);
    {
        auto opened = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(opened.hasValue());
        REQUIRE(opened.value()->execute("CREATE TABLE state(revision INTEGER)").hasValue());
        const std::array parameter {Value {std::int64_t {42}}};
        REQUIRE(opened.value()->execute("INSERT INTO state(revision) VALUES(?)", parameter).hasValue());
    }
    {
        auto reopened = SqlitePersistenceBackend::open(SqliteConnectionOptions {path});
        REQUIRE(reopened.hasValue());
        auto rows = reopened.value()->query("SELECT revision FROM state");
        REQUIRE(rows.hasValue());
        REQUIRE(rows.value().size() == 1U);
        CHECK(*rows.value().front().at("revision").getIf<std::int64_t>() == 42);
    }
    removeFile(path);
}

TEST_CASE("SqlitePersistenceBackend validates connection options", "[infrastructure][sqlite]")
{
    auto emptyPath = SqlitePersistenceBackend::open(SqliteConnectionOptions {});
    REQUIRE_FALSE(emptyPath.hasValue());
    CHECK(std::string(emptyPath.error().code.value()) == "Persistence.InvalidOptions");

    SqliteConnectionOptions negativeTimeout {":memory:"};
    negativeTimeout.busyTimeoutMilliseconds = -1;
    auto invalidTimeout = SqlitePersistenceBackend::open(negativeTimeout);
    REQUIRE_FALSE(invalidTimeout.hasValue());
    CHECK(std::string(invalidTimeout.error().code.value()) == "Persistence.InvalidOptions");

    const auto missingParent = uniqueDatabasePath() / "database.db";
    auto failedOpen = SqlitePersistenceBackend::open(SqliteConnectionOptions {missingParent});
    REQUIRE_FALSE(failedOpen.hasValue());
    CHECK(std::string(failedOpen.error().code.value()) == "Persistence.DatabaseOpenFailed");

    SqliteConnectionOptions foreignKeysDisabled {":memory:"};
    foreignKeysDisabled.enableForeignKeys = false;
    auto opened = SqlitePersistenceBackend::open(foreignKeysDisabled);
    REQUIRE(opened.hasValue());
    auto foreignKeys = opened.value()->query("PRAGMA foreign_keys");
    REQUIRE(foreignKeys.hasValue());
    REQUIRE(foreignKeys.value().size() == 1U);
    CHECK(*foreignKeys.value().front().at("foreign_keys").getIf<std::int64_t>() == 0);
}
