#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
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
