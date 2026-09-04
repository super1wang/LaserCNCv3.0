#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>
#include "../../file_path_validation.hpp"

#include <lasercnc/foundation/error.hpp>

#include <sqlite3.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cctype>
#include <cstdint>
#include <limits>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lasercnc::infrastructure {
namespace {

using StatementPtr = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

std::string pathToUtf8(const std::filesystem::path& path)
{
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

foundation::Error databaseError(
    const char* code,
    const char* message,
    sqlite3* database,
    int resultCode)
{
    const int extendedCode = database == nullptr ? resultCode : sqlite3_extended_errcode(database);
    const char* reason = database == nullptr ? sqlite3_errstr(resultCode) : sqlite3_errmsg(database);
    return foundation::makeError(
        code,
        foundation::ErrorCategory::Infrastructure,
        message,
        foundation::Value {foundation::Value::Object {
            {"backend", foundation::Value {"sqlite"}},
            {"resultCode", foundation::Value {static_cast<std::int64_t>(resultCode)}},
            {"extendedResultCode", foundation::Value {static_cast<std::int64_t>(extendedCode)}},
            {"reason", foundation::Value {reason == nullptr ? "Unknown SQLite failure" : reason}},
        }});
}

foundation::Error validationError(const char* code, const char* message, std::string reason)
{
    return foundation::makeError(
        code,
        foundation::ErrorCategory::Validation,
        message,
        foundation::Value {foundation::Value::Object {
            {"backend", foundation::Value {"sqlite"}},
            {"reason", foundation::Value {std::move(reason)}},
        }});
}

foundation::Error transactionConflict(const char* message)
{
    return foundation::makeError(
        "Persistence.TransactionStateConflict",
        foundation::ErrorCategory::Conflict,
        message,
        foundation::Value {foundation::Value::Object {
            {"backend", foundation::Value {"sqlite"}},
        }});
}

bool containsOnlyWhitespace(const char* begin, const char* end) noexcept
{
    while(begin != end) {
        if(std::isspace(static_cast<unsigned char>(*begin)) == 0) {
            return false;
        }
        ++begin;
    }
    return true;
}

foundation::Result<void> bindValue(
    sqlite3* database,
    sqlite3_stmt* statement,
    int index,
    const foundation::Value& value)
{
    int resultCode = SQLITE_ERROR;
    switch(value.kind()) {
    case foundation::Value::Kind::Null:
        resultCode = sqlite3_bind_null(statement, index);
        break;
    case foundation::Value::Kind::Boolean:
        resultCode = sqlite3_bind_int(statement, index, *value.getIf<bool>() ? 1 : 0);
        break;
    case foundation::Value::Kind::Integer:
        resultCode = sqlite3_bind_int64(
            statement, index, static_cast<sqlite3_int64>(*value.getIf<std::int64_t>()));
        break;
    case foundation::Value::Kind::Number:
        resultCode = sqlite3_bind_double(statement, index, *value.getIf<double>());
        break;
    case foundation::Value::Kind::String: {
        const auto& text = *value.getIf<std::string>();
        resultCode = sqlite3_bind_text64(
            statement,
            index,
            text.data(),
            static_cast<sqlite3_uint64>(text.size()),
            SQLITE_TRANSIENT,
            SQLITE_UTF8);
        break;
    }
    case foundation::Value::Kind::Array:
    case foundation::Value::Kind::Object:
        return foundation::Result<void>::failure(validationError(
            "Persistence.UnsupportedParameter",
            "SQLite parameters must be scalar Kernel values",
            "Array and Object values require an explicit repository serialization policy"));
    }

    if(resultCode != SQLITE_OK) {
        return foundation::Result<void>::failure(databaseError(
            "Persistence.DatabaseFailed", "SQLite could not bind a statement parameter", database, resultCode));
    }
    return foundation::Result<void>::success();
}

foundation::Result<foundation::Value> readColumn(
    sqlite3* database,
    sqlite3_stmt* statement,
    int column)
{
    switch(sqlite3_column_type(statement, column)) {
    case SQLITE_NULL:
        return foundation::Result<foundation::Value>::success(foundation::Value {});
    case SQLITE_INTEGER:
        return foundation::Result<foundation::Value>::success(foundation::Value {
            static_cast<std::int64_t>(sqlite3_column_int64(statement, column))});
    case SQLITE_FLOAT:
        return foundation::Result<foundation::Value>::success(
            foundation::Value {sqlite3_column_double(statement, column)});
    case SQLITE_TEXT: {
        const auto* bytes = sqlite3_column_text(statement, column);
        const int size = sqlite3_column_bytes(statement, column);
        if(size < 0 || (bytes == nullptr && size != 0)) {
            return foundation::Result<foundation::Value>::failure(databaseError(
                "Persistence.DatabaseFailed", "SQLite returned invalid text data", database, SQLITE_ERROR));
        }
        return foundation::Result<foundation::Value>::success(foundation::Value {std::string(
            reinterpret_cast<const char*>(bytes == nullptr
                                              ? reinterpret_cast<const unsigned char*>("")
                                              : bytes),
            static_cast<std::size_t>(size))});
    }
    case SQLITE_BLOB:
        return foundation::Result<foundation::Value>::failure(validationError(
            "Persistence.UnsupportedColumnType",
            "SQLite BLOB columns are outside the control-plane Value contract",
            "Store large or binary assets in the data plane and persist only metadata"));
    default:
        return foundation::Result<foundation::Value>::failure(databaseError(
            "Persistence.DatabaseFailed", "SQLite returned an unknown column type", database, SQLITE_ERROR));
    }
}

} // namespace

class SqlitePersistenceBackend::Impl final {
public:
    explicit Impl(sqlite3* database) noexcept
        : database_(database)
    {
    }

    ~Impl()
    {
        if(database_ != nullptr) {
            if(sqlite3_get_autocommit(database_) == 0) {
                static_cast<void>(sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr));
            }
            static_cast<void>(sqlite3_close_v2(database_));
        }
    }

    foundation::Result<void> acquireFileOwnership()
    {
        if(hostFileOwned_) { return foundation::Result<void>::success(); }
        HANDLE handle = INVALID_HANDLE_VALUE;
        const int code = sqlite3_file_control(database_, "main", SQLITE_FCNTL_WIN32_GET_HANDLE, &handle);
        if(code != SQLITE_OK || handle == nullptr || handle == INVALID_HANDLE_VALUE) {
            return foundation::Result<void>::failure(databaseError(
                "Persistence.HostSessionUnsupported", "The SQLite VFS does not expose its native file handle",
                database_, code));
        }
        // Resolve the actual SQLite handle, not another pathname lookup or a sidecar lock file.
        // 中文翻译：从 SQLite 实际句柄解析路径，避免二次按路径打开或旁路锁文件遗漏别名与替换竞态。
        std::vector<wchar_t> path(32768U);
        const DWORD size = GetFinalPathNameByHandleW(handle, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED);
        std::vector<wchar_t> volume(32768U);
        wchar_t filesystem[32]{};
        if(size == 0U || size >= path.size()
           || !GetVolumePathNameW(path.data(), volume.data(), static_cast<DWORD>(volume.size()))
           || GetDriveTypeW(volume.data()) != DRIVE_FIXED
           || !GetVolumeInformationW(volume.data(), nullptr, 0U, nullptr, nullptr, nullptr,
               filesystem, static_cast<DWORD>(std::size(filesystem)))
           || (std::wstring_view(filesystem) != L"NTFS" && std::wstring_view(filesystem) != L"ReFS")) {
            return foundation::Result<void>::failure(foundation::makeError(
                "Persistence.HostSessionStorageUnsupported", foundation::ErrorCategory::Validation,
                "Host sessions require a local fixed NTFS or ReFS volume"));
        }
        // This byte is beyond SQLite's maximum database size and its native locking range.
        // 中文翻译：该字节超出 SQLite 最大数据库大小及其内部锁区；锁定不会写入或扩大文件。
        OVERLAPPED range{};
        range.Offset = 0xfffffffeU;
        range.OffsetHigh = 0x7fffffffU;
        if(!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0U, 1U, 0U, &range)) {
            const auto error = GetLastError();
            return foundation::Result<void>::failure(foundation::makeError(
                error == ERROR_LOCK_VIOLATION ? "Persistence.HostAlreadyOwned" : "Persistence.HostSessionLockFailed",
                error == ERROR_LOCK_VIOLATION ? foundation::ErrorCategory::Conflict : foundation::ErrorCategory::Infrastructure,
                "Exclusive persistence Host ownership could not be acquired",
                foundation::Value{foundation::Value::Object{{"systemCode", foundation::Value{static_cast<std::int64_t>(error)}}}}));
        }
        // SQLite owns the handle. Its close (including process termination) releases this lock.
        // 中文翻译：句柄由 SQLite 持有；连接关闭或进程退出释放锁，不在 commit/rollback/shutdown 时提前释放。
        hostFileOwned_ = true;
        return foundation::Result<void>::success();
    }

    foundation::Result<foundation::Value> pragma(std::string_view sql)
    {
        auto statement = prepare(sql, {});
        if(!statement) { return foundation::Result<foundation::Value>::failure(std::move(statement).error()); }
        const int step = sqlite3_step(statement.value().get());
        if(step != SQLITE_ROW || sqlite3_column_count(statement.value().get()) != 1) {
            return foundation::Result<foundation::Value>::failure(databaseError(
                "Persistence.HostSessionPolicyReadFailed", "SQLite session policy returned no unique scalar",
                database_, step));
        }
        auto value = readColumn(database_, statement.value().get(), 0);
        const int end = sqlite3_step(statement.value().get());
        if(end != SQLITE_DONE) {
            return foundation::Result<foundation::Value>::failure(databaseError(
                "Persistence.HostSessionPolicyReadFailed", "SQLite session policy returned extra rows or failed",
                database_, end));
        }
        return value;
    }

    foundation::Result<platform::PersistenceSessionInfo> acquireSession()
    {
        if(sqlite3_get_autocommit(database_) == 0) {
            return foundation::Result<platform::PersistenceSessionInfo>::failure(transactionConflict(
                "Host session admission is forbidden inside a transaction"));
        }
        const auto fail = [](foundation::Error error) {
            return foundation::Result<platform::PersistenceSessionInfo>::failure(std::move(error));
        };
        if(!session_.has_value()) {
            if(!privateMemory_) {
                auto owned = acquireFileOwnership();
                if(!owned) { return fail(std::move(owned).error()); }
            }
            auto journal = pragma(privateMemory_ ? "PRAGMA main.journal_mode=MEMORY" : "PRAGMA main.journal_mode=DELETE");
            if(!journal) { return fail(std::move(journal).error()); }
            for(const auto* sql : {"PRAGMA main.synchronous=EXTRA", "PRAGMA foreign_keys=ON"}) {
                auto set = executeUnlocked(sql, {});
                if(!set) { return fail(std::move(set).error()); }
            }
        }
        // Read back the effective policy rather than assuming a successful PRAGMA changed it.
        // 中文翻译：读回实际生效策略，不把 PRAGMA 未报错视作已生效。
        auto journal = pragma("PRAGMA main.journal_mode");
        auto synchronous = pragma("PRAGMA main.synchronous");
        auto foreignKeys = pragma("PRAGMA foreign_keys");
        auto pageSize = pragma("PRAGMA main.page_size");
        auto cacheSize = pragma("PRAGMA main.cache_size");
        if(!journal) { return fail(std::move(journal).error()); }
        if(!synchronous) { return fail(std::move(synchronous).error()); }
        if(!foreignKeys) { return fail(std::move(foreignKeys).error()); }
        if(!pageSize) { return fail(std::move(pageSize).error()); }
        if(!cacheSize) { return fail(std::move(cacheSize).error()); }
        if(journal.value() != foundation::Value{privateMemory_ ? "memory" : "delete"}
           || synchronous.value() != foundation::Value{std::int64_t{3}}
           || foreignKeys.value() != foundation::Value{std::int64_t{1}}) {
            return fail(foundation::makeError("Persistence.HostSessionPolicyMismatch",
                foundation::ErrorCategory::Infrastructure, "The effective SQLite Host session policy is incompatible"));
        }
        session_ = platform::PersistenceSessionInfo{"sqlite", !privateMemory_,
            foundation::Value{foundation::Value::Object{
                {"journalMode", std::move(journal).value()},
                {"synchronous", std::move(synchronous).value()},
                {"foreignKeys", std::move(foreignKeys).value()},
                {"pageSize", std::move(pageSize).value()},
                {"cacheSize", std::move(cacheSize).value()},
                {"ownership", foundation::Value{privateMemory_ ? "private-memory" : "native-file-range-lock-v1"}},
            }}};
        return foundation::Result<platform::PersistenceSessionInfo>::success(*session_);
    }

    foundation::Result<StatementPtr> prepare(
        std::string_view sql,
        std::span<const foundation::Value> parameters)
    {
        if(sql.empty()) {
            return foundation::Result<StatementPtr>::failure(validationError(
                "Persistence.InvalidStatement", "The SQLite statement is invalid", "Statement is empty"));
        }
        if(sql.find('\0') != std::string_view::npos) {
            return foundation::Result<StatementPtr>::failure(validationError(
                "Persistence.InvalidStatement",
                "The SQLite statement is invalid",
                "Embedded null characters are not allowed"));
        }
        if(sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return foundation::Result<StatementPtr>::failure(validationError(
                "Persistence.InvalidStatement",
                "The SQLite statement is invalid",
                "Statement exceeds the SQLite size limit"));
        }
        if(parameters.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return foundation::Result<StatementPtr>::failure(validationError(
                "Persistence.ParameterCountMismatch",
                "The SQLite parameter count is invalid",
                "Parameter count exceeds the SQLite limit"));
        }

        sqlite3_stmt* rawStatement = nullptr;
        const char* tail = nullptr;
        const int resultCode = sqlite3_prepare_v2(
            database_, sql.data(), static_cast<int>(sql.size()), &rawStatement, &tail);
        StatementPtr statement(rawStatement, sqlite3_finalize);
        if(resultCode != SQLITE_OK) {
            return foundation::Result<StatementPtr>::failure(databaseError(
                "Persistence.DatabaseFailed", "SQLite could not prepare the statement", database_, resultCode));
        }
        if(rawStatement == nullptr) {
            return foundation::Result<StatementPtr>::failure(validationError(
                "Persistence.InvalidStatement",
                "The SQLite statement is invalid",
                "Statement contains no executable SQL"));
        }
        const char* end = sql.data() + sql.size();
        if(tail == nullptr || !containsOnlyWhitespace(tail, end)) {
            return foundation::Result<StatementPtr>::failure(validationError(
                "Persistence.MultipleStatementsDenied",
                "Only one SQLite statement may be executed per call",
                "Non-whitespace content follows the first statement"));
        }

        const int expectedParameters = sqlite3_bind_parameter_count(rawStatement);
        if(expectedParameters != static_cast<int>(parameters.size())) {
            return foundation::Result<StatementPtr>::failure(validationError(
                "Persistence.ParameterCountMismatch",
                "The SQLite parameter count does not match the statement",
                "Every placeholder must have exactly one Kernel Value"));
        }
        for(std::size_t index = 0; index < parameters.size(); ++index) {
            auto bound = bindValue(
                database_, rawStatement, static_cast<int>(index) + 1, parameters[index]);
            if(!bound.hasValue()) {
                return foundation::Result<StatementPtr>::failure(std::move(bound).error());
            }
        }
        return foundation::Result<StatementPtr>::success(std::move(statement));
    }

    foundation::Result<std::size_t> executeUnlocked(
        std::string_view sql,
        std::span<const foundation::Value> parameters)
    {
        auto prepared = prepare(sql, parameters);
        if(!prepared.hasValue()) {
            return foundation::Result<std::size_t>::failure(std::move(prepared).error());
        }

        if(sqlite3_column_count(prepared.value().get()) != 0) {
            return foundation::Result<std::size_t>::failure(validationError(
                "Persistence.UnexpectedRows",
                "The execute operation would produce rows",
                "Use query for statements that return rows"));
        }

        const sqlite3_int64 totalChangesBefore = sqlite3_total_changes64(database_);
        const int resultCode = sqlite3_step(prepared.value().get());
        if(resultCode != SQLITE_DONE) {
            return foundation::Result<std::size_t>::failure(databaseError(
                "Persistence.DatabaseFailed", "SQLite could not execute the statement", database_, resultCode));
        }

        const sqlite3_int64 totalChangesAfter = sqlite3_total_changes64(database_);
        if(totalChangesBefore < 0 || totalChangesAfter < totalChangesBefore) {
            return foundation::Result<std::size_t>::failure(databaseError(
                "Persistence.DatabaseFailed", "SQLite returned an invalid affected-row count", database_, SQLITE_ERROR));
        }
        const auto changedRows = static_cast<std::uint64_t>(totalChangesAfter - totalChangesBefore);
        if(changedRows
                  > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            return foundation::Result<std::size_t>::failure(databaseError(
                "Persistence.DatabaseFailed", "SQLite returned an invalid affected-row count", database_, SQLITE_ERROR));
        }
        return foundation::Result<std::size_t>::success(static_cast<std::size_t>(changedRows));
    }

    sqlite3* database_{nullptr};
    std::mutex mutex_;
    bool privateMemory_{false};
    bool hostFileOwned_{false};
    std::optional<platform::PersistenceSessionInfo> session_;
};

SqlitePersistenceBackend::SqlitePersistenceBackend(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation))
{
}

SqlitePersistenceBackend::~SqlitePersistenceBackend() = default;

foundation::Result<std::unique_ptr<SqlitePersistenceBackend>> SqlitePersistenceBackend::open(
    SqliteConnectionOptions options)
{
    if(options.databasePath.empty() || detail::containsEmbeddedNull(options.databasePath)) {
        return foundation::Result<std::unique_ptr<SqlitePersistenceBackend>>::failure(validationError(
            "Persistence.InvalidOptions",
            "The SQLite connection options are invalid",
            "Database path must be non-empty and contain no embedded null characters"));
    }
    if(options.busyTimeoutMilliseconds < 0) {
        return foundation::Result<std::unique_ptr<SqlitePersistenceBackend>>::failure(validationError(
            "Persistence.InvalidOptions",
            "The SQLite connection options are invalid",
            "Busy timeout must not be negative"));
    }

    sqlite3* database = nullptr;
    const auto path = pathToUtf8(options.databasePath);
    int resultCode = sqlite3_open_v2(
        path.c_str(),
        &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if(resultCode != SQLITE_OK) {
        auto error = databaseError(
            "Persistence.DatabaseOpenFailed", "SQLite could not open the database", database, resultCode);
        if(database != nullptr) {
            static_cast<void>(sqlite3_close_v2(database));
        }
        return foundation::Result<std::unique_ptr<SqlitePersistenceBackend>>::failure(std::move(error));
    }

    auto implementation = std::make_unique<Impl>(database);
    implementation->privateMemory_ = options.databasePath == std::filesystem::path{":memory:"};
    resultCode = sqlite3_extended_result_codes(database, 1);
    if(resultCode == SQLITE_OK) {
        resultCode = sqlite3_busy_timeout(database, options.busyTimeoutMilliseconds);
    }
    if(resultCode == SQLITE_OK) {
        const char* pragma = options.enableForeignKeys ? "PRAGMA foreign_keys = ON"
                                                       : "PRAGMA foreign_keys = OFF";
        resultCode = sqlite3_exec(database, pragma, nullptr, nullptr, nullptr);
    }
    if(resultCode != SQLITE_OK) {
        return foundation::Result<std::unique_ptr<SqlitePersistenceBackend>>::failure(databaseError(
            "Persistence.DatabaseOpenFailed",
            "SQLite connection initialization failed",
            database,
            resultCode));
    }

    return foundation::Result<std::unique_ptr<SqlitePersistenceBackend>>::success(
        std::unique_ptr<SqlitePersistenceBackend>(
            new SqlitePersistenceBackend(std::move(implementation))));
}

foundation::Result<platform::PersistenceSessionInfo> SqlitePersistenceBackend::acquireHostSession()
{
    std::lock_guard lock(implementation_->mutex_);
    return implementation_->acquireSession();
}

foundation::Result<std::size_t> SqlitePersistenceBackend::execute(
    std::string_view statement,
    std::span<const foundation::Value> parameters)
{
    std::lock_guard lock(implementation_->mutex_);
    return implementation_->executeUnlocked(statement, parameters);
}

foundation::Result<std::vector<platform::PersistenceRow>> SqlitePersistenceBackend::query(
    std::string_view statement,
    std::span<const foundation::Value> parameters)
{
    std::lock_guard lock(implementation_->mutex_);
    auto prepared = implementation_->prepare(statement, parameters);
    if(!prepared.hasValue()) {
        return foundation::Result<std::vector<platform::PersistenceRow>>::failure(
            std::move(prepared).error());
    }
    if(sqlite3_column_count(prepared.value().get()) == 0) {
        return foundation::Result<std::vector<platform::PersistenceRow>>::failure(validationError(
            "Persistence.NoResultColumns",
            "The query operation requires a row-producing statement",
            "Use execute for statements that do not return columns"));
    }

    std::vector<platform::PersistenceRow> rows;
    while(true) {
        const int resultCode = sqlite3_step(prepared.value().get());
        if(resultCode == SQLITE_DONE) {
            return foundation::Result<std::vector<platform::PersistenceRow>>::success(
                std::move(rows));
        }
        if(resultCode != SQLITE_ROW) {
            return foundation::Result<std::vector<platform::PersistenceRow>>::failure(databaseError(
                "Persistence.DatabaseFailed",
                "SQLite could not query the database",
                implementation_->database_,
                resultCode));
        }

        platform::PersistenceRow row;
        const int columnCount = sqlite3_column_count(prepared.value().get());
        for(int column = 0; column < columnCount; ++column) {
            const char* rawName = sqlite3_column_name(prepared.value().get(), column);
            if(rawName == nullptr) {
                return foundation::Result<std::vector<platform::PersistenceRow>>::failure(databaseError(
                    "Persistence.DatabaseFailed",
                    "SQLite returned a column without a name",
                    implementation_->database_,
                    SQLITE_ERROR));
            }
            auto value = readColumn(
                implementation_->database_, prepared.value().get(), column);
            if(!value.hasValue()) {
                return foundation::Result<std::vector<platform::PersistenceRow>>::failure(
                    std::move(value).error());
            }
            const auto [unused, inserted] = row.emplace(rawName, std::move(value).value());
            static_cast<void>(unused);
            if(!inserted) {
                return foundation::Result<std::vector<platform::PersistenceRow>>::failure(validationError(
                    "Persistence.DuplicateColumnName",
                    "SQLite query columns must have unique names",
                    "Alias duplicate result columns before querying through the Kernel port"));
            }
        }
        rows.push_back(std::move(row));
    }
}

foundation::Result<void> SqlitePersistenceBackend::beginTransaction()
{
    std::lock_guard lock(implementation_->mutex_);
    if(sqlite3_get_autocommit(implementation_->database_) == 0) {
        return foundation::Result<void>::failure(
            transactionConflict("A SQLite transaction is already active"));
    }
    if(implementation_->session_.has_value()) {
        auto verified = implementation_->acquireSession();
        if(!verified) { return foundation::Result<void>::failure(std::move(verified).error()); }
    }
    auto result = implementation_->executeUnlocked("BEGIN IMMEDIATE", {});
    if(!result.hasValue()) {
        return foundation::Result<void>::failure(std::move(result).error());
    }
    return foundation::Result<void>::success();
}

foundation::Result<void> SqlitePersistenceBackend::commitTransaction()
{
    std::lock_guard lock(implementation_->mutex_);
    if(sqlite3_get_autocommit(implementation_->database_) != 0) {
        return foundation::Result<void>::failure(
            transactionConflict("No SQLite transaction is active"));
    }
    auto result = implementation_->executeUnlocked("COMMIT", {});
    if(!result.hasValue()) {
        return foundation::Result<void>::failure(std::move(result).error());
    }
    return foundation::Result<void>::success();
}

foundation::Result<void> SqlitePersistenceBackend::rollbackTransaction()
{
    std::lock_guard lock(implementation_->mutex_);
    if(sqlite3_get_autocommit(implementation_->database_) != 0) {
        return foundation::Result<void>::failure(
            transactionConflict("No SQLite transaction is active"));
    }
    auto result = implementation_->executeUnlocked("ROLLBACK", {});
    if(!result.hasValue()) {
        return foundation::Result<void>::failure(std::move(result).error());
    }
    return foundation::Result<void>::success();
}

} // namespace lasercnc::infrastructure
