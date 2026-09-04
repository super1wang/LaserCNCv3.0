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

// This fixture is a Host; the previous Host must be destroyed before opening it.
// 中文翻译：本夹具本身也是 Host，打开前必须销毁同一数据库的前任 Host。
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

// Deliberately corrupt a live database using raw SQL, outside the trusted Host contract.
// 中文翻译：仅负向测试使用原始 SQL 注入损坏材料；这是可信 Host 契约外的破坏，不是第二 Host 准入。
inline void injectJournalFixture(const std::filesystem::path& database,
    const runtime::TransactionCommit& commit)
{
    auto encoded = openPersistenceFixture(":memory:");
    // Encode a valid template, then explicitly forge the requested revision metadata for negative tests.
    // 中文翻译：先编码合法模板，再明确伪造负向测试所需修订；不绕过正式写入准入。
    auto templateCommit = commit;
    templateCommit.revisionsBefore = {};
    templateCommit.revisionsAfter = {state::Revision{1U}, state::Revision{1U}, {}, {}, {}, {}};
    auto appended = encoded->append(templateCommit);
    if(!appended) { throw std::runtime_error(appended.error().message); }
    auto record = std::move(appended).value();
    infrastructure::JsonconsAdapter serializer;
    auto decoded = serializer.deserialize(record.payload);
    if(!decoded) { throw std::runtime_error(decoded.error().message); }
    const auto revisionsValue = [](const state::RevisionSet& revisions) {
        foundation::Value::Object value;
        for(const auto scope : {state::RevisionScope::Project, state::RevisionScope::Document,
                state::RevisionScope::Geometry, state::RevisionScope::Cam,
                state::RevisionScope::MachineContext, state::RevisionScope::Environment}) {
            value.emplace(state::revisionScopeName(scope), foundation::Value{std::to_string(revisions.at(scope).value())});
        }
        return foundation::Value{std::move(value)};
    };
    auto& payload = *decoded.value().getIf<foundation::Value::Object>();
    payload.insert_or_assign("revisionsBefore", revisionsValue(commit.revisionsBefore));
    payload.insert_or_assign("revisionsAfter", revisionsValue(commit.revisionsAfter));
    auto serialized = serializer.serialize(decoded.value());
    if(!serialized) { throw std::runtime_error(serialized.error().message); }
    record.payload = std::move(serialized).value();
    auto digest = infrastructure::Sha256HashService{}.digest(std::as_bytes(std::span{record.payload.data(), record.payload.size()}));
    if(!digest) { throw std::runtime_error(digest.error().message); }
    record.digest = std::move(digest).value();
    record.revisionsBefore = commit.revisionsBefore;
    record.revisionsAfter = commit.revisionsAfter;
    std::vector<foundation::Value> parameters{
        foundation::Value{std::string(record.transactionId.value())},
        foundation::Value{std::string(record.projectId.value())},
        foundation::Value{std::string(record.documentId.value())}};
    for(const auto& revisions : {record.revisionsBefore, record.revisionsAfter}) {
        for(const auto scope : {state::RevisionScope::Project, state::RevisionScope::Document,
                state::RevisionScope::Geometry, state::RevisionScope::Cam,
                state::RevisionScope::MachineContext, state::RevisionScope::Environment}) {
            parameters.emplace_back(std::to_string(revisions.at(scope).value()));
        }
    }
    parameters.emplace_back(record.payload);
    parameters.emplace_back(std::string(record.digest.value()));
    parameters.emplace_back(static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        record.committedAt.time_since_epoch()).count()));
    auto backend = infrastructure::SqlitePersistenceBackend::open({database});
    if(!backend) { throw std::runtime_error(backend.error().message); }
    auto inserted = backend.value()->execute("INSERT INTO state_journal("
        "transaction_id,project_id,document_id,project_revision_before,document_revision_before,"
        "geometry_revision_before,cam_revision_before,machine_context_revision_before,environment_revision_before,"
        "project_revision_after,document_revision_after,geometry_revision_after,cam_revision_after,"
        "machine_context_revision_after,environment_revision_after,payload,digest,committed_at_ms)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", parameters);
    if(!inserted) { throw std::runtime_error(inserted.error().message); }
}

} // namespace lasercnc::test
