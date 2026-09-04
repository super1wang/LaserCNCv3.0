#pragma once

#include <lasercnc/infrastructure/filesystem_snapshot_store.hpp>
#include <lasercnc/infrastructure/jsoncons_adapter.hpp>
#include <lasercnc/infrastructure/sha256_hash_service.hpp>
#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>
#include <lasercnc/persistence/persistence_service.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <utility>

namespace lasercnc::test {

// This fixture owns a separate database connection, never a mutable AppKernel service.
// 中文翻译：夹具拥有独立数据库连接，不取得 AppKernel 的可变服务。
inline std::unique_ptr<persistence::PersistenceService> openPersistenceFixture(
    const std::filesystem::path& database,
    const std::filesystem::path& snapshots = {},
    std::size_t maximumSnapshotBytes = 1024U * 1024U)
{
    auto backend = infrastructure::SqlitePersistenceBackend::open({database});
    if(!backend) { throw std::runtime_error(backend.error().message); }
    std::unique_ptr<platform::ISnapshotStore> store;
    if(!snapshots.empty()) {
        auto opened = infrastructure::FilesystemSnapshotStore::create({snapshots, maximumSnapshotBytes});
        if(!opened) { throw std::runtime_error(opened.error().message); }
        store = std::move(opened).value();
    }
    auto service = std::make_unique<persistence::PersistenceService>();
    auto configured = service->configure(std::move(backend).value(),
        std::make_shared<infrastructure::JsonconsAdapter>(),
        std::make_shared<infrastructure::Sha256HashService>(), std::move(store));
    if(!configured) { throw std::runtime_error(configured.error().message); }
    auto initialized = service->initialize();
    if(!initialized) { throw std::runtime_error(initialized.error().message); }
    return service;
}

} // namespace lasercnc::test
