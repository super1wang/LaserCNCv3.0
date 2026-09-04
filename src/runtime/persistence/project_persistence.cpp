#include <lasercnc/persistence/persistence_service.hpp>

#include <array>
#include <exception>
#include <set>
#include <string_view>
#include <utility>

namespace lasercnc::persistence {
namespace {

using foundation::Error;
using foundation::ErrorCategory;
using foundation::Result;
using foundation::Value;

Error projectError(const char* code, const char* message,
                   ErrorCategory category = ErrorCategory::Infrastructure)
{
    return foundation::makeError(code, category, message);
}

std::span<const std::byte> bytes(std::string_view value) noexcept
{
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

constexpr std::array states {ProjectPersistenceState::Closed, ProjectPersistenceState::Opening,
    ProjectPersistenceState::Open, ProjectPersistenceState::Closing, ProjectPersistenceState::Failed};

const char* stateName(ProjectPersistenceState state) noexcept
{
    switch(state) {
    case ProjectPersistenceState::Closed: return "closed";
    case ProjectPersistenceState::Opening: return "opening";
    case ProjectPersistenceState::Open: return "open";
    case ProjectPersistenceState::Closing: return "closing";
    case ProjectPersistenceState::Failed: return "failed";
    }
    return nullptr;
}

template<typename T>
Result<T> column(const platform::PersistenceRow& row, std::string_view name)
{
    const auto found = row.find(name);
    const auto* value = found == row.end() ? nullptr : found->second.getIf<T>();
    if(value == nullptr) {
        return Result<T>::failure(projectError("Persistence.InvalidProjectCatalogColumn",
            "A project catalog column is missing or has an invalid type"));
    }
    return Result<T>::success(*value);
}

Result<bool> migrationPending(platform::IPersistenceBackend& backend)
{
    auto rows = backend.query("SELECT singleton,completed FROM project_catalog_migration");
    if(!rows) { return Result<bool>::failure(std::move(rows).error()); }
    if(rows.value().size() != 1U) {
        return Result<bool>::failure(projectError("Persistence.InvalidProjectCatalogMigration",
            "The project catalog migration marker is missing or ambiguous"));
    }
    auto singleton = column<std::int64_t>(rows.value().front(), "singleton");
    auto completed = column<std::int64_t>(rows.value().front(), "completed");
    if(!singleton || !completed || singleton.value() != 1
       || (completed.value() != 0 && completed.value() != 1)) {
        return Result<bool>::failure(projectError("Persistence.InvalidProjectCatalogMigration",
            "The project catalog migration marker is invalid"));
    }
    return Result<bool>::success(completed.value() == 0);
}

Result<void> requireMigrated(platform::IPersistenceBackend& backend)
{
    auto pending = migrationPending(backend);
    if(!pending) { return Result<void>::failure(std::move(pending).error()); }
    if(pending.value()) {
        return Result<void>::failure(projectError("Persistence.ProjectCatalogMigrationRequired",
            "Verified legacy project identities must be migrated before project catalog use",
            ErrorCategory::Conflict));
    }
    return Result<void>::success();
}

Value recordValue(const kernel::ProjectId& project, ProjectPersistenceState state, std::int64_t time)
{
    return Value {Value::Object {
        {"format", Value {"lasercnc.project-lifecycle.v1"}},
        {"projectId", Value {std::string(project.value())}},
        {"state", Value {stateName(state)}},
        {"updatedAtMs", Value {time}}}};
}

Result<ProjectCatalogRecord> decode(const platform::PersistenceRow& row,
    foundation::IValueSerializer& serializer, platform::IHashService& hashes)
{
    auto idText = column<std::string>(row, "project_id");
    auto stateText = column<std::string>(row, "state");
    auto payload = column<std::string>(row, "payload");
    auto digestText = column<std::string>(row, "digest");
    auto time = column<std::int64_t>(row, "updated_at_ms");
    if(!idText || !stateText || !payload || !digestText || !time) {
        return Result<ProjectCatalogRecord>::failure(projectError("Persistence.InvalidProjectCatalogRecord",
            "The project catalog record has missing or invalid columns"));
    }
    auto id = kernel::ProjectId::create(idText.value());
    auto digest = kernel::ContentDigest::create(digestText.value());
    std::optional<ProjectPersistenceState> state;
    for(const auto candidate : states) {
        if(stateText.value() == stateName(candidate)) { state = candidate; break; }
    }
    const auto maximumTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::time_point::max().time_since_epoch()).count();
    if(!id || !digest || !state || time.value() < 0 || time.value() > maximumTime) {
        return Result<ProjectCatalogRecord>::failure(projectError("Persistence.InvalidProjectCatalogRecord",
            "The project catalog identity, lifecycle state, digest or timestamp is invalid"));
    }
    auto actual = hashes.digest(bytes(payload.value()));
    auto decoded = serializer.deserialize(payload.value());
    if(!actual) { return Result<ProjectCatalogRecord>::failure(std::move(actual).error()); }
    if(!decoded) { return Result<ProjectCatalogRecord>::failure(std::move(decoded).error()); }
    if(actual.value() != digest.value() || decoded.value() != recordValue(id.value(), *state, time.value())) {
        return Result<ProjectCatalogRecord>::failure(projectError("Persistence.ProjectCatalogIntegrityFailed",
            "The project catalog payload does not match its digest and control columns"));
    }
    const bool interrupted = *state == ProjectPersistenceState::Opening || *state == ProjectPersistenceState::Closing;
    return Result<ProjectCatalogRecord>::success(ProjectCatalogRecord {std::move(id).value(),
        interrupted ? ProjectPersistenceState::Failed : *state, interrupted,
        std::chrono::system_clock::time_point {std::chrono::milliseconds {time.value()}}});
}

Result<void> writeRecord(platform::IPersistenceBackend& backend,
    foundation::IValueSerializer& serializer, platform::IHashService& hashes,
    const kernel::ProjectId& project, ProjectPersistenceState state)
{
    const auto time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto payload = serializer.serialize(recordValue(project, state, time));
    if(!payload) { return Result<void>::failure(std::move(payload).error()); }
    auto digest = hashes.digest(bytes(payload.value()));
    if(!digest) { return Result<void>::failure(std::move(digest).error()); }
    const std::array parameters {Value {std::string(project.value())}, Value {stateName(state)},
        Value {payload.value()}, Value {std::string(digest.value().value())}, Value {static_cast<std::int64_t>(time)}};
    auto written = backend.execute(
        "INSERT INTO project_catalog(project_id,state,payload,digest,updated_at_ms) VALUES(?,?,?,?,?) "
        "ON CONFLICT(project_id) DO UPDATE SET state=excluded.state,payload=excluded.payload,"
        "digest=excluded.digest,updated_at_ms=excluded.updated_at_ms", parameters);
    if(!written) { return Result<void>::failure(std::move(written).error()); }
    if(written.value() != 1U) {
        return Result<void>::failure(projectError("Persistence.ProjectCatalogWriteCountInvalid",
            "Project catalog persistence did not affect exactly one record"));
    }
    return Result<void>::success();
}

Result<void> rollback(platform::IPersistenceBackend& backend, Error primary)
{
    try {
        auto result = backend.rollbackTransaction();
        if(result) { return Result<void>::failure(std::move(primary)); }
    } catch(...) {
        // QuarantiningBackend owns connection quarantine; no operation may revive it.
        // 中文翻译：连接隔离由 QuarantiningBackend 负责，不能在这里重新激活。
    }
    return Result<void>::failure(foundation::makeError("Persistence.ProjectCatalogRollbackFailed",
        ErrorCategory::Infrastructure, "Project catalog persistence failed and rollback did not complete",
        Value {}, foundation::Severity::Error, std::make_shared<const Error>(std::move(primary))));
}

template<typename Operation>
Result<void> transaction(platform::IPersistenceBackend& backend, Operation operation)
{
    bool open = false;
    try {
        auto begun = backend.beginTransaction();
        if(!begun) { return begun; }
        open = true;
        auto result = operation();
        if(!result) { return rollback(backend, std::move(result).error()); }
        auto committed = backend.commitTransaction();
        if(!committed) { return rollback(backend, std::move(committed).error()); }
        return Result<void>::success();
    } catch(...) {
        auto error = projectError("Persistence.ProjectCatalogWriteFailed",
            "Project catalog persistence failed unexpectedly", ErrorCategory::Internal);
        return open ? rollback(backend, std::move(error)) : Result<void>::failure(std::move(error));
    }
}

Error notReady()
{
    return projectError("Persistence.NotReady", "Persistence must be initialized before project catalog use",
        ErrorCategory::Conflict);
}

} // namespace

Result<bool> PersistenceService::projectCatalogMigrationPending() const
{
    std::lock_guard lock(mutex_);
    if(!initialized_) { return Result<bool>::failure(notReady()); }
    try { return migrationPending(*backend_); }
    catch(...) {
        return Result<bool>::failure(projectError("Persistence.ProjectCatalogReadFailed",
            "Project catalog migration metadata could not be read", ErrorCategory::Internal));
    }
}

Result<void> PersistenceService::completeProjectCatalogMigration(
    std::span<const kernel::ProjectId> verifiedLegacyProjects)
{
    std::lock_guard lock(mutex_);
    if(!initialized_) { return Result<void>::failure(notReady()); }
    return transaction(*backend_, [&]() -> Result<void> {
        auto pending = migrationPending(*backend_);
        if(!pending) { return Result<void>::failure(std::move(pending).error()); }
        if(!pending.value()) { return Result<void>::success(); }
        const std::set<kernel::ProjectId> verified(verifiedLegacyProjects.begin(), verifiedLegacyProjects.end());
        if(verified.size() != verifiedLegacyProjects.size()) {
            return Result<void>::failure(projectError("Persistence.DuplicateLegacyProjectIdentity",
                "Verified legacy project identities must be unique", ErrorCategory::Validation));
        }
        auto existing = backend_->query("SELECT project_id FROM project_catalog");
        if(!existing) { return Result<void>::failure(std::move(existing).error()); }
        if(!existing.value().empty()) {
            return Result<void>::failure(projectError("Persistence.ProjectCatalogMigrationConflict",
                "An incomplete project migration already contains project records"));
        }
        auto sources = backend_->query("SELECT project_id FROM document_catalog UNION "
            "SELECT project_id FROM state_journal UNION SELECT project_id FROM snapshot_index");
        if(!sources) { return Result<void>::failure(std::move(sources).error()); }
        std::set<kernel::ProjectId> expected;
        for(const auto& row : sources.value()) {
            auto text = column<std::string>(row, "project_id");
            if(!text) { return Result<void>::failure(std::move(text).error()); }
            auto id = kernel::ProjectId::create(text.value());
            if(!id) { return Result<void>::failure(std::move(id).error()); }
            expected.insert(std::move(id).value());
        }
        if(expected != verified) {
            return Result<void>::failure(projectError("Persistence.LegacyProjectSetMismatch",
                "Verified project identities do not match all durable document ownership sources",
                ErrorCategory::Conflict));
        }
        for(const auto& id : verified) {
            auto written = writeRecord(*backend_, *serializer_, *hashes_, id, ProjectPersistenceState::Open);
            if(!written) { return written; }
        }
        auto completed = backend_->execute(
            "UPDATE project_catalog_migration SET completed=1 WHERE singleton=1 AND completed=0");
        if(!completed) { return Result<void>::failure(std::move(completed).error()); }
        if(completed.value() != 1U) {
            return Result<void>::failure(projectError("Persistence.InvalidProjectCatalogMigration",
                "The project catalog migration completion marker was not updated exactly once"));
        }
        return Result<void>::success();
    });
}

Result<void> PersistenceService::saveProjectLifecycle(const kernel::ProjectId& projectId, ProjectPersistenceState state)
{
    std::lock_guard lock(mutex_);
    if(!initialized_) { return Result<void>::failure(notReady()); }
    if(stateName(state) == nullptr) {
        return Result<void>::failure(projectError("Persistence.InvalidProjectLifecycleState",
            "The project lifecycle state is unknown", ErrorCategory::Validation));
    }
    return transaction(*backend_, [&]() -> Result<void> {
        auto admitted = requireMigrated(*backend_);
        if(!admitted) { return admitted; }
        const std::array parameters {Value {std::string(projectId.value())}};
        auto rows = backend_->query("SELECT project_id,state,payload,digest,updated_at_ms FROM project_catalog "
            "WHERE project_id=?", parameters);
        if(!rows) { return Result<void>::failure(std::move(rows).error()); }
        if(rows.value().size() > 1U) {
            return Result<void>::failure(projectError("Persistence.DuplicateProjectCatalogIdentity",
                "The project catalog contains duplicate stable identities"));
        }
        if(!rows.value().empty()) {
            auto previous = decode(rows.value().front(), *serializer_, *hashes_);
            if(!previous) { return Result<void>::failure(std::move(previous).error()); }
            if(previous.value().projectId != projectId) {
                return Result<void>::failure(projectError("Persistence.ProjectCatalogIntegrityFailed",
                    "Project catalog lookup returned another stable identity"));
            }
        }
        return writeRecord(*backend_, *serializer_, *hashes_, projectId, state);
    });
}

Result<std::vector<ProjectCatalogRecord>> PersistenceService::projectCatalog() const
{
    std::lock_guard lock(mutex_);
    if(!initialized_) { return Result<std::vector<ProjectCatalogRecord>>::failure(notReady()); }
    try {
        auto admitted = requireMigrated(*backend_);
        if(!admitted) { return Result<std::vector<ProjectCatalogRecord>>::failure(std::move(admitted).error()); }
        auto rows = backend_->query("SELECT project_id,state,payload,digest,updated_at_ms FROM project_catalog ORDER BY project_id");
        if(!rows) { return Result<std::vector<ProjectCatalogRecord>>::failure(std::move(rows).error()); }
        std::vector<ProjectCatalogRecord> result;
        std::set<kernel::ProjectId> identities;
        for(const auto& row : rows.value()) {
            auto record = decode(row, *serializer_, *hashes_);
            if(!record) { return Result<std::vector<ProjectCatalogRecord>>::failure(std::move(record).error()); }
            if(!identities.insert(record.value().projectId).second) {
                return Result<std::vector<ProjectCatalogRecord>>::failure(projectError("Persistence.DuplicateProjectCatalogIdentity",
                    "The project catalog contains duplicate stable identities"));
            }
            result.push_back(std::move(record).value());
        }
        return Result<std::vector<ProjectCatalogRecord>>::success(std::move(result));
    } catch(...) {
        return Result<std::vector<ProjectCatalogRecord>>::failure(projectError("Persistence.ProjectCatalogReadFailed",
            "Project catalog recovery failed unexpectedly", ErrorCategory::Internal));
    }
}

} // namespace lasercnc::persistence
