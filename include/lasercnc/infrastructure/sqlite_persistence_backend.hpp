#pragma once

#include <lasercnc/platform/persistence_backend.hpp>

#include <filesystem>
#include <memory>

namespace lasercnc::infrastructure {

struct SqliteConnectionOptions final {
    std::filesystem::path databasePath;
    int busyTimeoutMilliseconds{5'000};
    bool enableForeignKeys{true};
};

class SqlitePersistenceBackend final : public platform::IPersistenceBackend {
public:
    [[nodiscard]] static foundation::Result<std::unique_ptr<SqlitePersistenceBackend>> open(
        SqliteConnectionOptions options);

    ~SqlitePersistenceBackend() override;

    SqlitePersistenceBackend(const SqlitePersistenceBackend&) = delete;
    SqlitePersistenceBackend& operator=(const SqlitePersistenceBackend&) = delete;
    SqlitePersistenceBackend(SqlitePersistenceBackend&&) = delete;
    SqlitePersistenceBackend& operator=(SqlitePersistenceBackend&&) = delete;

    [[nodiscard]] foundation::Result<platform::PersistenceSessionInfo> acquireHostSession() override;

    [[nodiscard]] foundation::Result<std::size_t> execute(
        std::string_view statement,
        std::span<const foundation::Value> parameters = {}) override;
    [[nodiscard]] foundation::Result<std::vector<platform::PersistenceRow>> query(
        std::string_view statement,
        std::span<const foundation::Value> parameters = {}) override;
    [[nodiscard]] foundation::Result<void> beginTransaction() override;
    [[nodiscard]] foundation::Result<void> commitTransaction() override;
    [[nodiscard]] foundation::Result<void> rollbackTransaction() override;

private:
    class Impl;

    explicit SqlitePersistenceBackend(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace lasercnc::infrastructure
