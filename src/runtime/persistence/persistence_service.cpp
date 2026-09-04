#include "object_record_codec.hpp"

#include <lasercnc/persistence/persistence_service.hpp>

#include <lasercnc/foundation/error.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace lasercnc::persistence {
namespace {

constexpr std::array revisionScopes {
    state::RevisionScope::Project,
    state::RevisionScope::Document,
    state::RevisionScope::Geometry,
    state::RevisionScope::Cam,
    state::RevisionScope::MachineContext,
    state::RevisionScope::Environment,
};

constexpr std::array revisionColumns {
    "project_revision",
    "document_revision",
    "geometry_revision",
    "cam_revision",
    "machine_context_revision",
    "environment_revision",
};

foundation::Error persistenceError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    foundation::Value::Object details = {},
    std::shared_ptr<const foundation::Error> cause = nullptr)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {std::move(details)},
        foundation::Severity::Error,
        std::move(cause));
}

foundation::Value revisionsValue(const state::RevisionSet& revisions)
{
    foundation::Value::Object result;
    for(const auto scope : revisionScopes) {
        result.emplace(
            state::revisionScopeName(scope),
            foundation::Value {std::to_string(revisions.at(scope).value())});
    }
    return foundation::Value {std::move(result)};
}

foundation::Value objectValue(const state::ObjectRecord& object)
{
    return detail::encodeObjectRecord(object);
}

const char* changeKindName(runtime::ObjectChangeKind kind) noexcept
{
    switch(kind) {
    case runtime::ObjectChangeKind::Created: return "created";
    case runtime::ObjectChangeKind::Updated: return "updated";
    case runtime::ObjectChangeKind::Removed: return "removed";
    }
    return "unknown";
}

const char* historyKindName(runtime::HistoryMutationKind kind) noexcept
{
    switch(kind) {
    case runtime::HistoryMutationKind::None: return "none";
    case runtime::HistoryMutationKind::Record: return "record";
    case runtime::HistoryMutationKind::Barrier: return "barrier";
    case runtime::HistoryMutationKind::Undo: return "undo";
    case runtime::HistoryMutationKind::Redo: return "redo";
    }
    return "unknown";
}

foundation::Value historyValue(const runtime::HistoryMutation& history)
{
    foundation::Value commandVersion;
    if(history.commandVersion.has_value()) {
        commandVersion = foundation::Value {foundation::Value::Object {
            {"major", foundation::Value {
                static_cast<std::int64_t>(history.commandVersion->major)}},
            {"minor", foundation::Value {
                static_cast<std::int64_t>(history.commandVersion->minor)}},
            {"patch", foundation::Value {
                static_cast<std::int64_t>(history.commandVersion->patch)}},
        }};
    }
    return foundation::Value {foundation::Value::Object {
        {"command", history.command.has_value()
            ? foundation::Value {std::string(history.command->value())}
            : foundation::Value {}},
        {"commandVersion", std::move(commandVersion)},
        {"expectedCursor", history.expectedCursor.has_value()
            ? foundation::Value {std::to_string(*history.expectedCursor)}
            : foundation::Value {}},
        {"kind", foundation::Value {historyKindName(history.kind)}},
        {"targetTransactionId", history.targetTransactionId.has_value()
            ? foundation::Value {std::string(history.targetTransactionId->value())}
            : foundation::Value {}},
    }};
}

foundation::Value commitValue(const runtime::TransactionCommit& commit)
{
    foundation::Value::Array changes;
    changes.reserve(commit.changes.size());
    for(const auto& change : commit.changes) {
        changes.emplace_back(foundation::Value::Object {
            {"after", change.after.has_value() ? objectValue(*change.after) : foundation::Value {}},
            {"before", change.before.has_value() ? objectValue(*change.before) : foundation::Value {}},
            {"kind", foundation::Value {changeKindName(change.kind)}},
            {"objectId", foundation::Value {std::string(change.objectId.value())}},
        });
    }

    foundation::Value::Array events;
    events.reserve(commit.events.size());
    for(const auto& event : commit.events) {
        events.emplace_back(foundation::Value::Object {
            {"aggregateId",
             event.aggregateId().has_value()
                 ? foundation::Value {std::string(event.aggregateId()->value())}
                 : foundation::Value {}},
            {"name", foundation::Value {std::string(event.name().value())}},
            {"payload", event.payload()},
            {"sequence", foundation::Value {static_cast<std::int64_t>(event.sequence())}},
            {"version", foundation::Value {foundation::Value::Object {
                {"major", foundation::Value {static_cast<std::int64_t>(event.version().major)}},
                {"minor", foundation::Value {static_cast<std::int64_t>(event.version().minor)}},
                {"patch", foundation::Value {static_cast<std::int64_t>(event.version().patch)}},
            }}},
        });
    }

    return foundation::Value {foundation::Value::Object {
        {"changes", foundation::Value {std::move(changes)}},
        {"documentId", foundation::Value {std::string(commit.documentId.value())}},
        {"events", foundation::Value {std::move(events)}},
        {"format", foundation::Value {"lasercnc.state-journal"}},
        {"history", historyValue(commit.history)},
        {"projectId", foundation::Value {std::string(commit.projectId.value())}},
        {"revisionsAfter", revisionsValue(commit.revisionsAfter)},
        {"revisionsBefore", revisionsValue(commit.revisionsBefore)},
        {"transactionId", foundation::Value {std::string(commit.transactionId.value())}},
        {"version", foundation::Value {std::int64_t {4}}},
    }};
}

std::span<const std::byte> bytes(std::string_view value) noexcept
{
    return {
        reinterpret_cast<const std::byte*>(value.data()),
        value.size()};
}

foundation::Result<void> rollback(
    platform::IPersistenceBackend& backend,
    foundation::Error primary)
{
    auto rolledBack = backend.rollbackTransaction();
    if(rolledBack) {
        return foundation::Result<void>::failure(std::move(primary));
    }
    auto rollbackError = std::move(rolledBack).error();
    return foundation::Result<void>::failure(persistenceError(
        "Persistence.RollbackFailed",
        foundation::ErrorCategory::Infrastructure,
        "Persistence failed and its database transaction could not be rolled back",
        {{"rollbackCode", foundation::Value {std::string(rollbackError.code.value())}},
         {"rollbackMessage", foundation::Value {rollbackError.message}}},
        std::make_shared<const foundation::Error>(std::move(primary))));
}

foundation::Result<std::string> textColumn(
    const platform::PersistenceRow& row,
    const char* name)
{
    const auto found = row.find(name);
    const auto* text = found == row.end() ? nullptr : found->second.getIf<std::string>();
    if(text == nullptr) {
        return foundation::Result<std::string>::failure(persistenceError(
            "Persistence.InvalidJournalRow",
            foundation::ErrorCategory::Infrastructure,
            "A persisted journal row contains a missing or invalid text column",
            {{"column", foundation::Value {name}}}));
    }
    return foundation::Result<std::string>::success(*text);
}

foundation::Result<std::int64_t> integerColumn(
    const platform::PersistenceRow& row,
    const char* name)
{
    const auto found = row.find(name);
    const auto* value = found == row.end() ? nullptr : found->second.getIf<std::int64_t>();
    if(value == nullptr) {
        return foundation::Result<std::int64_t>::failure(persistenceError(
            "Persistence.InvalidJournalRow",
            foundation::ErrorCategory::Infrastructure,
            "A persisted journal row contains a missing or invalid integer column",
            {{"column", foundation::Value {name}}}));
    }
    return foundation::Result<std::int64_t>::success(*value);
}

foundation::Result<state::Revision> parseRevision(std::string_view text)
{
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if(parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size()) {
        return foundation::Result<state::Revision>::failure(persistenceError(
            "Persistence.InvalidJournalRow",
            foundation::ErrorCategory::Infrastructure,
            "A persisted journal revision is invalid"));
    }
    return foundation::Result<state::Revision>::success(state::Revision {value});
}

foundation::Result<state::RevisionSet> revisionsFromRow(
    const platform::PersistenceRow& row,
    const char* suffix)
{
    std::array<state::Revision, revisionScopes.size()> values;
    for(std::size_t index = 0; index < revisionScopes.size(); ++index) {
        auto text = textColumn(row, (std::string(revisionColumns[index]) + suffix).c_str());
        if(!text) {
            return foundation::Result<state::RevisionSet>::failure(std::move(text).error());
        }
        auto revision = parseRevision(text.value());
        if(!revision) {
            return foundation::Result<state::RevisionSet>::failure(std::move(revision).error());
        }
        values[index] = revision.value();
    }
    return foundation::Result<state::RevisionSet>::success(state::RevisionSet {
        values[0], values[1], values[2], values[3], values[4], values[5]});
}

template <typename Id>
foundation::Result<Id> idColumn(const platform::PersistenceRow& row, const char* name)
{
    auto text = textColumn(row, name);
    if(!text) {
        return foundation::Result<Id>::failure(std::move(text).error());
    }
    auto id = Id::create(std::move(text).value());
    if(!id) {
        return foundation::Result<Id>::failure(persistenceError(
            "Persistence.InvalidJournalRow",
            foundation::ErrorCategory::Infrastructure,
            "A persisted journal row contains an invalid stable identity",
            {{"column", foundation::Value {name}}},
            std::make_shared<const foundation::Error>(std::move(id).error())));
    }
    return id;
}

foundation::Result<JournalRecord> recordFromRow(const platform::PersistenceRow& row)
{
    auto sequence = integerColumn(row, "sequence");
    auto transactionId = idColumn<kernel::TransactionId>(row, "transaction_id");
    auto projectId = idColumn<kernel::ProjectId>(row, "project_id");
    auto documentId = idColumn<kernel::DocumentId>(row, "document_id");
    auto before = revisionsFromRow(row, "_before");
    auto after = revisionsFromRow(row, "_after");
    auto payload = textColumn(row, "payload");
    auto digest = idColumn<kernel::ContentDigest>(row, "digest");
    auto committedAt = integerColumn(row, "committed_at_ms");
    if(!sequence || !transactionId || !projectId || !documentId || !before || !after
       || !payload || !digest || !committedAt || sequence.value() <= 0
       || committedAt.value() < 0) {
        return foundation::Result<JournalRecord>::failure(persistenceError(
            "Persistence.InvalidJournalRow",
            foundation::ErrorCategory::Infrastructure,
            "A persisted journal row is invalid"));
    }
    return foundation::Result<JournalRecord>::success(JournalRecord {
        static_cast<std::uint64_t>(sequence.value()),
        std::move(transactionId).value(),
        std::move(projectId).value(),
        std::move(documentId).value(),
        std::move(before).value(),
        std::move(after).value(),
        std::move(payload).value(),
        std::move(digest).value(),
        std::chrono::system_clock::time_point {
            std::chrono::milliseconds {committedAt.value()}}});
}

foundation::Result<void> validateRecord(
    const JournalRecord& record,
    const foundation::IValueSerializer& serializer,
    const platform::IHashService& hashes)
{
    auto digest = hashes.digest(bytes(record.payload));
    if(!digest) {
        return foundation::Result<void>::failure(std::move(digest).error());
    }
    if(digest.value() != record.digest) {
        return foundation::Result<void>::failure(persistenceError(
            "Persistence.JournalDigestMismatch",
            foundation::ErrorCategory::Infrastructure,
            "A persisted journal payload failed its content digest check",
            {{"sequence", foundation::Value {std::to_string(record.sequence)}}}));
    }
    auto decoded = serializer.deserialize(record.payload);
    if(!decoded) {
        return foundation::Result<void>::failure(persistenceError(
            "Persistence.JournalPayloadInvalid",
            foundation::ErrorCategory::Infrastructure,
            "A persisted journal payload cannot be decoded",
            {},
            std::make_shared<const foundation::Error>(std::move(decoded).error())));
    }
    const auto* root = decoded.value().getIf<foundation::Value::Object>();
    if(root == nullptr) {
        return foundation::Result<void>::failure(persistenceError(
            "Persistence.JournalPayloadInvalid",
            foundation::ErrorCategory::Infrastructure,
            "A persisted journal payload must be an object"));
    }
    const auto matchesText = [root](const char* name, std::string_view expected) {
        const auto found = root->find(name);
        const auto* text = found == root->end() ? nullptr : found->second.getIf<std::string>();
        return text != nullptr && *text == expected;
    };
    const auto version = root->find("version");
    const auto* versionNumber = version == root->end()
        ? nullptr
        : version->second.getIf<std::int64_t>();
    const auto before = root->find("revisionsBefore");
    const auto after = root->find("revisionsAfter");
    const auto history = root->find("history");
    if(!matchesText("format", "lasercnc.state-journal")
       || versionNumber == nullptr || (*versionNumber != 1 && *versionNumber != 2 && *versionNumber != 3 && *versionNumber != 4)
       || (*versionNumber >= 2
           && (history == root->end()
               || history->second.kind() != foundation::Value::Kind::Object))
       || !matchesText("transactionId", record.transactionId.value())
       || !matchesText("projectId", record.projectId.value())
       || !matchesText("documentId", record.documentId.value())
       || before == root->end() || before->second != revisionsValue(record.revisionsBefore)
       || after == root->end() || after->second != revisionsValue(record.revisionsAfter)) {
        return foundation::Result<void>::failure(persistenceError(
            "Persistence.JournalMetadataMismatch",
            foundation::ErrorCategory::Infrastructure,
            "A persisted journal payload does not match its control-plane metadata",
            {{"sequence", foundation::Value {std::to_string(record.sequence)}}}));
    }
    return foundation::Result<void>::success();
}

constexpr std::string_view selectColumns =
    "sequence,transaction_id,project_id,document_id,"
    "project_revision_before,document_revision_before,geometry_revision_before,"
    "cam_revision_before,machine_context_revision_before,environment_revision_before,"
    "project_revision_after,document_revision_after,geometry_revision_after,"
    "cam_revision_after,machine_context_revision_after,environment_revision_after,"
    "payload,digest,committed_at_ms";

// A failed rollback makes every later observation on this connection untrustworthy.
// 中文翻译：回滚失败后，该连接上的后续读写均不再可信，必须使用新实例恢复。
class QuarantiningBackend final : public platform::IPersistenceBackend {
public:
    QuarantiningBackend(std::unique_ptr<platform::IPersistenceBackend> delegate, bool& initialized)
        : delegate_(std::move(delegate)), initialized_(initialized) {}

    foundation::Result<std::size_t> execute(std::string_view sql,
        std::span<const foundation::Value> parameters) override
    {
        return failed_ ? unavailable<std::size_t>() : delegate_->execute(sql, parameters);
    }
    foundation::Result<std::vector<platform::PersistenceRow>> query(std::string_view sql,
        std::span<const foundation::Value> parameters) override
    {
        return failed_ ? unavailable<std::vector<platform::PersistenceRow>>() : delegate_->query(sql, parameters);
    }
    foundation::Result<void> beginTransaction() override
    {
        return failed_ ? unavailable<void>() : delegate_->beginTransaction();
    }
    foundation::Result<void> commitTransaction() override
    {
        return failed_ ? unavailable<void>() : delegate_->commitTransaction();
    }
    foundation::Result<void> rollbackTransaction() override
    {
        if(failed_) { return unavailable<void>(); }
        const bool wasInitialized = initialized_;
        failed_ = true;
        initialized_ = false;
        try {
            auto rolledBack = delegate_->rollbackTransaction();
            if(rolledBack) {
                failed_ = false;
                initialized_ = wasInitialized;
            }
            return rolledBack;
        } catch(...) {
            return foundation::Result<void>::failure(persistenceError(
                "Persistence.RollbackException", foundation::ErrorCategory::Infrastructure,
                "The persistence backend raised an exception during rollback"));
        }
    }

private:
    template <typename T>
    foundation::Result<T> unavailable() const
    {
        return foundation::Result<T>::failure(persistenceError(
            "Persistence.BackendQuarantined", foundation::ErrorCategory::Infrastructure,
            "Rollback could not be confirmed; discard this persistence instance and recover with a new one"));
    }
    std::unique_ptr<platform::IPersistenceBackend> delegate_;
    bool& initialized_;
    bool failed_{false};
};

} // namespace

foundation::Result<void> PersistenceService::configure(
    std::unique_ptr<platform::IPersistenceBackend> backend,
    std::shared_ptr<foundation::IValueSerializer> serializer,
    std::shared_ptr<platform::IHashService> hashes,
    std::unique_ptr<platform::ISnapshotStore> snapshotStore)
{
    if(backend == nullptr || serializer == nullptr || hashes == nullptr) {
        return foundation::Result<void>::failure(persistenceError(
            "Persistence.InvalidServices",
            foundation::ErrorCategory::Validation,
            "Persistence requires a backend, value serializer, and hash service"));
    }
    std::lock_guard lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::failure(persistenceError(
            "Persistence.ConfigurationFrozen",
            foundation::ErrorCategory::Conflict,
            "Persistence configuration is frozen"));
    }
    if(backend_ != nullptr) {
        return foundation::Result<void>::failure(persistenceError(
            "Persistence.AlreadyConfigured",
            foundation::ErrorCategory::Conflict,
            "Persistence services can only be configured once"));
    }
    backend_ = std::make_unique<QuarantiningBackend>(std::move(backend), initialized_);
    serializer_ = std::move(serializer);
    hashes_ = std::move(hashes);
    snapshotStore_ = std::move(snapshotStore);
    return foundation::Result<void>::success();
}

foundation::Result<void> PersistenceService::initialize()
{
    std::lock_guard lock(mutex_);
    if(backend_ == nullptr) {
        return foundation::Result<void>::failure(persistenceError(
            "Persistence.NotConfigured",
            foundation::ErrorCategory::Conflict,
            "Persistence services have not been configured"));
    }
    if(initialized_) {
        return foundation::Result<void>::success();
    }
    bool transactionOpen = false;
    try {
        auto begun = backend_->beginTransaction();
        if(!begun) {
            return begun;
        }
        transactionOpen = true;
        auto migrations = backend_->execute(
            "CREATE TABLE IF NOT EXISTS schema_migrations("
            "version INTEGER PRIMARY KEY NOT NULL, applied_at TEXT NOT NULL)");
        if(!migrations) {
            return rollback(*backend_, std::move(migrations).error());
        }
        auto versions = backend_->query(
            "SELECT COALESCE(MAX(version),0) AS version FROM schema_migrations");
        if(!versions || versions.value().size() != 1U) {
            return rollback(
                *backend_,
                versions ? persistenceError(
                    "Persistence.InvalidSchemaVersion",
                    foundation::ErrorCategory::Infrastructure,
                    "The persistence schema version query returned an invalid result")
                         : std::move(versions).error());
        }
        auto version = integerColumn(versions.value().front(), "version");
        if(!version) {
            return rollback(*backend_, std::move(version).error());
        }
        if(version.value() > 9) {
            return rollback(*backend_, persistenceError(
                "Persistence.SchemaTooNew",
                foundation::ErrorCategory::Conflict,
                "The persistence schema is newer than this kernel supports"));
        }
        auto journal = backend_->execute(
            "CREATE TABLE IF NOT EXISTS state_journal("
            "sequence INTEGER PRIMARY KEY AUTOINCREMENT,"
            "transaction_id TEXT NOT NULL UNIQUE,project_id TEXT NOT NULL,document_id TEXT NOT NULL,"
            "project_revision_before TEXT NOT NULL,document_revision_before TEXT NOT NULL,"
            "geometry_revision_before TEXT NOT NULL,cam_revision_before TEXT NOT NULL,"
            "machine_context_revision_before TEXT NOT NULL,environment_revision_before TEXT NOT NULL,"
            "project_revision_after TEXT NOT NULL,document_revision_after TEXT NOT NULL,"
            "geometry_revision_after TEXT NOT NULL,cam_revision_after TEXT NOT NULL,"
            "machine_context_revision_after TEXT NOT NULL,environment_revision_after TEXT NOT NULL,"
            "payload TEXT NOT NULL,digest TEXT NOT NULL,committed_at_ms INTEGER NOT NULL)");
        if(!journal) {
            return rollback(*backend_, std::move(journal).error());
        }
        auto index = backend_->execute(
            "CREATE INDEX IF NOT EXISTS idx_state_journal_document_sequence "
            "ON state_journal(document_id,sequence)");
        if(!index) {
            return rollback(*backend_, std::move(index).error());
        }
        auto snapshots = backend_->execute(
            "CREATE TABLE IF NOT EXISTS snapshot_index("
            "snapshot_id TEXT PRIMARY KEY NOT NULL,project_id TEXT NOT NULL,"
            "document_id TEXT NOT NULL,journal_sequence INTEGER NOT NULL,"
            "project_revision TEXT NOT NULL,document_revision TEXT NOT NULL,"
            "geometry_revision TEXT NOT NULL,cam_revision TEXT NOT NULL,"
            "machine_context_revision TEXT NOT NULL,environment_revision TEXT NOT NULL,"
            "storage_key TEXT NOT NULL,digest TEXT NOT NULL,payload_size INTEGER NOT NULL,"
            "created_at_ms INTEGER NOT NULL)");
        if(!snapshots) {
            return rollback(*backend_, std::move(snapshots).error());
        }
        auto snapshotIndex = backend_->execute(
            "CREATE INDEX IF NOT EXISTS idx_snapshot_document_sequence "
            "ON snapshot_index(document_id,journal_sequence DESC,created_at_ms DESC)");
        if(!snapshotIndex) {
            return rollback(*backend_, std::move(snapshotIndex).error());
        }
        auto idempotency = backend_->execute(
            "CREATE TABLE IF NOT EXISTS command_idempotency("
            "idempotency_key TEXT PRIMARY KEY NOT NULL,signature_payload TEXT NOT NULL,"
            "signature_digest TEXT NOT NULL,status TEXT NOT NULL,"
            "outcome_payload TEXT,outcome_digest TEXT,journal_transaction_id TEXT,"
            "task_id TEXT,created_at_ms INTEGER NOT NULL,completed_at_ms INTEGER)");
        if(!idempotency) {
            return rollback(*backend_, std::move(idempotency).error());
        }
        auto abandoned = backend_->execute(
            "UPDATE command_idempotency SET status='abandoned' WHERE status='pending'");
        if(!abandoned) {
            return rollback(*backend_, std::move(abandoned).error());
        }
        auto taskHistory = backend_->execute(
            "CREATE TABLE IF NOT EXISTS task_history("
            "task_id TEXT PRIMARY KEY NOT NULL,task_name TEXT NOT NULL,status TEXT NOT NULL,"
            "request_payload TEXT NOT NULL,request_digest TEXT NOT NULL,"
            "terminal_payload TEXT,terminal_digest TEXT,"
            "created_at_ms INTEGER NOT NULL,updated_at_ms INTEGER NOT NULL)");
        if(!taskHistory) {
            return rollback(*backend_, std::move(taskHistory).error());
        }
        auto interruptedTasks = backend_->execute(
            "UPDATE task_history SET status='interrupted' "
            "WHERE status IN ('accepted','running')");
        if(!interruptedTasks) {
            return rollback(*backend_, std::move(interruptedTasks).error());
        }
        auto diagnosticHistory = backend_->execute(
            "CREATE TABLE IF NOT EXISTS diagnostic_history("
            "sequence INTEGER PRIMARY KEY AUTOINCREMENT,diagnostic_id TEXT NOT NULL,"
            "status TEXT NOT NULL,payload TEXT NOT NULL,digest TEXT NOT NULL,"
            "observed_at_ms INTEGER NOT NULL)");
        if(!diagnosticHistory) {
            return rollback(*backend_, std::move(diagnosticHistory).error());
        }
        auto diagnosticIndex = backend_->execute(
            "CREATE INDEX IF NOT EXISTS idx_diagnostic_history_id_sequence "
            "ON diagnostic_history(diagnostic_id,sequence)");
        if(!diagnosticIndex) {
            return rollback(*backend_, std::move(diagnosticIndex).error());
        }
        auto workflowInstances = backend_->execute(
            "CREATE TABLE IF NOT EXISTS workflow_instances("
            "workflow_id TEXT PRIMARY KEY NOT NULL,workflow_name TEXT NOT NULL,"
            "workflow_major INTEGER NOT NULL,workflow_minor INTEGER NOT NULL,"
            "workflow_patch INTEGER NOT NULL,definition_digest TEXT NOT NULL,"
            "status TEXT NOT NULL,payload TEXT NOT NULL,digest TEXT NOT NULL,"
            "updated_at_ms INTEGER NOT NULL)");
        if(!workflowInstances) {
            return rollback(*backend_, std::move(workflowInstances).error());
        }
        auto workflowSteps = backend_->execute(
            "CREATE TABLE IF NOT EXISTS workflow_steps("
            "workflow_id TEXT NOT NULL,step_id TEXT NOT NULL,status TEXT NOT NULL,"
            "payload TEXT NOT NULL,digest TEXT NOT NULL,updated_at_ms INTEGER NOT NULL,"
            "PRIMARY KEY(workflow_id,step_id),"
            "FOREIGN KEY(workflow_id) REFERENCES workflow_instances(workflow_id) "
            "ON DELETE CASCADE)");
        if(!workflowSteps) {
            return rollback(*backend_, std::move(workflowSteps).error());
        }
        auto workflowStatusIndex = backend_->execute(
            "CREATE INDEX IF NOT EXISTS idx_workflow_instances_status_updated "
            "ON workflow_instances(status,updated_at_ms)");
        if(!workflowStatusIndex) {
            return rollback(*backend_, std::move(workflowStatusIndex).error());
        }
        auto externalEffects = backend_->execute(
            "CREATE TABLE IF NOT EXISTS external_effects("
            "idempotency_key TEXT PRIMARY KEY NOT NULL,signature_payload TEXT NOT NULL,"
            "signature_digest TEXT NOT NULL,replay_policy TEXT NOT NULL,state TEXT NOT NULL,"
            "outcome_payload TEXT,outcome_digest TEXT,started_at_ms INTEGER NOT NULL,"
            "updated_at_ms INTEGER NOT NULL)");
        if(!externalEffects) {
            return rollback(*backend_, std::move(externalEffects).error());
        }
        auto interruptedEffects = backend_->execute(
            "UPDATE external_effects SET state=CASE replay_policy "
            "WHEN 'safe' THEN 'interrupted' "
            "WHEN 'idempotent' THEN 'interrupted' "
            "WHEN 'reconcile_only' THEN 'reconcile_required' "
            "ELSE 'indeterminate' END "
            "WHERE state='executing'");
        if(!interruptedEffects) {
            return rollback(*backend_, std::move(interruptedEffects).error());
        }
        auto documentCatalog = backend_->execute(
            "CREATE TABLE IF NOT EXISTS document_catalog("
            "document_id TEXT PRIMARY KEY NOT NULL,project_id TEXT NOT NULL,"
            "state TEXT NOT NULL,payload TEXT NOT NULL,digest TEXT NOT NULL,"
            "updated_at_ms INTEGER NOT NULL)");
        if(!documentCatalog) {
            return rollback(*backend_, std::move(documentCatalog).error());
        }
        if(version.value() < 9) {
            auto projects = backend_->execute(
                "CREATE TABLE project_catalog("
                "project_id TEXT PRIMARY KEY NOT NULL,state TEXT NOT NULL,"
                "payload TEXT NOT NULL,digest TEXT NOT NULL,updated_at_ms INTEGER NOT NULL)");
            if(!projects) { return rollback(*backend_, std::move(projects).error()); }
            auto marker = backend_->execute(
                "CREATE TABLE project_catalog_migration("
                "singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
                "completed INTEGER NOT NULL CHECK(completed IN (0,1)))");
            if(!marker) { return rollback(*backend_, std::move(marker).error()); }
            auto pending = backend_->execute(
                "INSERT INTO project_catalog_migration(singleton,completed) VALUES(1,0)");
            if(!pending) { return rollback(*backend_, std::move(pending).error()); }
        } else {
            auto marker = backend_->query("SELECT singleton,completed FROM project_catalog_migration");
            if(!marker) { return rollback(*backend_, std::move(marker).error()); }
            if(marker.value().size() != 1U) {
                return rollback(*backend_, persistenceError(
                    "Persistence.InvalidProjectCatalogMigration", foundation::ErrorCategory::Infrastructure,
                    "The project catalog migration marker is missing or ambiguous"));
            }
            auto singleton = integerColumn(marker.value().front(), "singleton");
            auto completed = integerColumn(marker.value().front(), "completed");
            if(!singleton || !completed || singleton.value() != 1
               || (completed.value() != 0 && completed.value() != 1)) {
                return rollback(*backend_, persistenceError(
                    "Persistence.InvalidProjectCatalogMigration", foundation::ErrorCategory::Infrastructure,
                    "The project catalog migration marker is invalid"));
            }
            auto columns = backend_->query(
                "SELECT project_id,state,payload,digest,updated_at_ms FROM project_catalog LIMIT 0");
            if(!columns) { return rollback(*backend_, std::move(columns).error()); }
        }
        const std::array migrationParameters {foundation::Value {std::int64_t {1}}};
        auto recorded = backend_->execute(
            "INSERT OR IGNORE INTO schema_migrations(version,applied_at) "
            "VALUES(?,CURRENT_TIMESTAMP)",
            migrationParameters);
        if(!recorded) {
            return rollback(*backend_, std::move(recorded).error());
        }
        const std::array snapshotMigrationParameters {
            foundation::Value {std::int64_t {2}}};
        auto snapshotMigration = backend_->execute(
            "INSERT OR IGNORE INTO schema_migrations(version,applied_at) "
            "VALUES(?,CURRENT_TIMESTAMP)",
            snapshotMigrationParameters);
        if(!snapshotMigration) {
            return rollback(*backend_, std::move(snapshotMigration).error());
        }
        const std::array idempotencyMigrationParameters {
            foundation::Value {std::int64_t {3}}};
        auto idempotencyMigration = backend_->execute(
            "INSERT OR IGNORE INTO schema_migrations(version,applied_at) "
            "VALUES(?,CURRENT_TIMESTAMP)",
            idempotencyMigrationParameters);
        if(!idempotencyMigration) {
            return rollback(*backend_, std::move(idempotencyMigration).error());
        }
        const std::array taskMigrationParameters {
            foundation::Value {std::int64_t {4}}};
        auto taskMigration = backend_->execute(
            "INSERT OR IGNORE INTO schema_migrations(version,applied_at) "
            "VALUES(?,CURRENT_TIMESTAMP)",
            taskMigrationParameters);
        if(!taskMigration) {
            return rollback(*backend_, std::move(taskMigration).error());
        }
        const std::array diagnosticMigrationParameters {
            foundation::Value {std::int64_t {5}}};
        auto diagnosticMigration = backend_->execute(
            "INSERT OR IGNORE INTO schema_migrations(version,applied_at) "
            "VALUES(?,CURRENT_TIMESTAMP)",
            diagnosticMigrationParameters);
        if(!diagnosticMigration) {
            return rollback(*backend_, std::move(diagnosticMigration).error());
        }
        const std::array workflowMigrationParameters {
            foundation::Value {std::int64_t {6}}};
        auto workflowMigration = backend_->execute(
            "INSERT OR IGNORE INTO schema_migrations(version,applied_at) "
            "VALUES(?,CURRENT_TIMESTAMP)",
            workflowMigrationParameters);
        if(!workflowMigration) {
            return rollback(*backend_, std::move(workflowMigration).error());
        }
        const std::array externalEffectMigrationParameters {
            foundation::Value {std::int64_t {7}}};
        auto externalEffectMigration = backend_->execute(
            "INSERT OR IGNORE INTO schema_migrations(version,applied_at) "
            "VALUES(?,CURRENT_TIMESTAMP)",
            externalEffectMigrationParameters);
        if(!externalEffectMigration) {
            return rollback(*backend_, std::move(externalEffectMigration).error());
        }
        const std::array documentLifecycleMigrationParameters {
            foundation::Value {std::int64_t {8}}};
        auto documentLifecycleMigration = backend_->execute(
            "INSERT OR IGNORE INTO schema_migrations(version,applied_at) "
            "VALUES(?,CURRENT_TIMESTAMP)",
            documentLifecycleMigrationParameters);
        if(!documentLifecycleMigration) {
            return rollback(
                *backend_, std::move(documentLifecycleMigration).error());
        }
        const std::array projectMigrationParameters {foundation::Value {std::int64_t {9}}};
        auto projectMigration = backend_->execute(
            "INSERT OR IGNORE INTO schema_migrations(version,applied_at) VALUES(?,CURRENT_TIMESTAMP)",
            projectMigrationParameters);
        if(!projectMigration) { return rollback(*backend_, std::move(projectMigration).error()); }
        auto committed = backend_->commitTransaction();
        if(!committed) {
            return rollback(*backend_, std::move(committed).error());
        }
        transactionOpen = false;
        initialized_ = true;
        return foundation::Result<void>::success();
    } catch(const std::exception& exception) {
        auto error = persistenceError(
            "Persistence.InitializeFailed",
            foundation::ErrorCategory::Internal,
            "Persistence initialization failed unexpectedly",
            {{"reason", foundation::Value {exception.what()}}});
        return transactionOpen ? rollback(*backend_, std::move(error))
                               : foundation::Result<void>::failure(std::move(error));
    } catch(...) {
        auto error = persistenceError(
            "Persistence.InitializeFailed",
            foundation::ErrorCategory::Internal,
            "Persistence initialization failed unexpectedly");
        return transactionOpen ? rollback(*backend_, std::move(error))
                               : foundation::Result<void>::failure(std::move(error));
    }
}

foundation::Result<JournalRecord> PersistenceService::append(
    const runtime::TransactionCommit& commit,
    const std::optional<runtime::TransactionIdempotency>& idempotency)
{
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<JournalRecord>::failure(persistenceError(
            "Persistence.NotReady",
            foundation::ErrorCategory::Conflict,
            "Persistence must be initialized before appending journal records"));
    }
    bool transactionOpen = false;
    try {
        auto payload = serializer_->serialize(commitValue(commit));
        if(!payload) {
            return foundation::Result<JournalRecord>::failure(std::move(payload).error());
        }
        auto digest = hashes_->digest(bytes(payload.value()));
        if(!digest) {
            return foundation::Result<JournalRecord>::failure(std::move(digest).error());
        }
        const auto committedAt = std::chrono::system_clock::now();
        const auto committedAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            committedAt.time_since_epoch()).count();
        if(committedAtMs < 0) {
            return foundation::Result<JournalRecord>::failure(persistenceError(
                "Persistence.InvalidTimestamp",
                foundation::ErrorCategory::Internal,
                "The system clock produced an unsupported journal timestamp"));
        }

        auto begun = backend_->beginTransaction();
        if(!begun) {
            return foundation::Result<JournalRecord>::failure(std::move(begun).error());
        }
        transactionOpen = true;
        const std::array existingParameters {
            foundation::Value {std::string(commit.transactionId.value())}};
        auto existing = backend_->query(
            std::string("SELECT ") + std::string(selectColumns)
                + " FROM state_journal WHERE transaction_id=?",
            existingParameters);
        if(!existing) {
            auto failure = rollback(*backend_, std::move(existing).error());
            return foundation::Result<JournalRecord>::failure(std::move(failure).error());
        }
        if(!existing.value().empty()) {
            if(existing.value().size() != 1U) {
                auto failure = rollback(*backend_, persistenceError(
                    "Persistence.InvalidJournalRow",
                    foundation::ErrorCategory::Infrastructure,
                    "A journal transaction identity resolved ambiguously"));
                return foundation::Result<JournalRecord>::failure(std::move(failure).error());
            }
            auto record = recordFromRow(existing.value().front());
            if(!record) {
                auto failure = rollback(*backend_, std::move(record).error());
                return foundation::Result<JournalRecord>::failure(
                    std::move(failure).error());
            }
            auto valid = validateRecord(record.value(), *serializer_, *hashes_);
            if(!valid) {
                auto failure = rollback(*backend_, std::move(valid).error());
                return foundation::Result<JournalRecord>::failure(
                    std::move(failure).error());
            }
            if(record.value().payload != payload.value()
               || record.value().digest != digest.value()
               || record.value().projectId != commit.projectId
               || record.value().documentId != commit.documentId
               || record.value().revisionsBefore != commit.revisionsBefore
               || record.value().revisionsAfter != commit.revisionsAfter) {
                auto failure = rollback(*backend_, persistenceError(
                    "Persistence.JournalTransactionConflict",
                    foundation::ErrorCategory::Conflict,
                    "A transaction identity is already bound to different journal content"));
                return foundation::Result<JournalRecord>::failure(std::move(failure).error());
            }
            if(idempotency.has_value()) {
                auto completed = completeCommandInOpenTransaction(commit, *idempotency);
                if(!completed) {
                    auto failure = rollback(*backend_, std::move(completed).error());
                    return foundation::Result<JournalRecord>::failure(
                        std::move(failure).error());
                }
            }
            auto committed = backend_->commitTransaction();
            if(!committed) {
                auto failure = rollback(*backend_, std::move(committed).error());
                return foundation::Result<JournalRecord>::failure(
                    std::move(failure).error());
            }
            transactionOpen = false;
            return record;
        }

        std::vector<foundation::Value> parameters;
        parameters.reserve(18U);
        parameters.emplace_back(std::string(commit.transactionId.value()));
        parameters.emplace_back(std::string(commit.projectId.value()));
        parameters.emplace_back(std::string(commit.documentId.value()));
        for(const auto scope : revisionScopes) {
            parameters.emplace_back(std::to_string(commit.revisionsBefore.at(scope).value()));
        }
        for(const auto scope : revisionScopes) {
            parameters.emplace_back(std::to_string(commit.revisionsAfter.at(scope).value()));
        }
        parameters.emplace_back(payload.value());
        parameters.emplace_back(std::string(digest.value().value()));
        parameters.emplace_back(static_cast<std::int64_t>(committedAtMs));
        auto inserted = backend_->execute(
            "INSERT INTO state_journal("
            "transaction_id,project_id,document_id,"
            "project_revision_before,document_revision_before,geometry_revision_before,"
            "cam_revision_before,machine_context_revision_before,environment_revision_before,"
            "project_revision_after,document_revision_after,geometry_revision_after,"
            "cam_revision_after,machine_context_revision_after,environment_revision_after,"
            "payload,digest,committed_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            parameters);
        if(!inserted) {
            auto failure = rollback(*backend_, std::move(inserted).error());
            return foundation::Result<JournalRecord>::failure(std::move(failure).error());
        }
        auto rows = backend_->query(
            std::string("SELECT ") + std::string(selectColumns)
                + " FROM state_journal WHERE transaction_id=?",
            existingParameters);
        if(!rows || rows.value().size() != 1U) {
            auto failure = rollback(
                *backend_,
                rows ? persistenceError(
                    "Persistence.InvalidJournalRow",
                    foundation::ErrorCategory::Infrastructure,
                    "The appended journal record cannot be read back")
                     : std::move(rows).error());
            return foundation::Result<JournalRecord>::failure(std::move(failure).error());
        }
        auto record = recordFromRow(rows.value().front());
        if(!record) {
            auto failure = rollback(*backend_, std::move(record).error());
            return foundation::Result<JournalRecord>::failure(std::move(failure).error());
        }
        auto valid = validateRecord(record.value(), *serializer_, *hashes_);
        if(!valid) {
            auto failure = rollback(*backend_, std::move(valid).error());
            return foundation::Result<JournalRecord>::failure(std::move(failure).error());
        }
        if(idempotency.has_value()) {
            auto completed = completeCommandInOpenTransaction(commit, *idempotency);
            if(!completed) {
                auto failure = rollback(*backend_, std::move(completed).error());
                return foundation::Result<JournalRecord>::failure(
                    std::move(failure).error());
            }
        }
        auto committed = backend_->commitTransaction();
        if(!committed) {
            auto failure = rollback(*backend_, std::move(committed).error());
            return foundation::Result<JournalRecord>::failure(std::move(failure).error());
        }
        transactionOpen = false;
        return record;
    } catch(const std::exception& exception) {
        auto error = persistenceError(
            "Persistence.JournalAppendFailed",
            foundation::ErrorCategory::Internal,
            "The journal append failed unexpectedly",
            {{"reason", foundation::Value {exception.what()}}});
        if(transactionOpen) {
            auto failure = rollback(*backend_, std::move(error));
            return foundation::Result<JournalRecord>::failure(std::move(failure).error());
        }
        return foundation::Result<JournalRecord>::failure(std::move(error));
    } catch(...) {
        auto error = persistenceError(
            "Persistence.JournalAppendFailed",
            foundation::ErrorCategory::Internal,
            "The journal append failed unexpectedly");
        if(transactionOpen) {
            auto failure = rollback(*backend_, std::move(error));
            return foundation::Result<JournalRecord>::failure(std::move(failure).error());
        }
        return foundation::Result<JournalRecord>::failure(std::move(error));
    }
}

foundation::Result<std::vector<JournalRecord>> PersistenceService::journalAfter(
    const kernel::DocumentId& documentId,
    std::uint64_t sequence) const
{
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<std::vector<JournalRecord>>::failure(persistenceError(
            "Persistence.NotReady",
            foundation::ErrorCategory::Conflict,
            "Persistence must be initialized before reading journal records"));
    }
    if(sequence > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return foundation::Result<std::vector<JournalRecord>>::failure(persistenceError(
            "Persistence.InvalidJournalSequence",
            foundation::ErrorCategory::Validation,
            "The journal sequence exceeds the backend range"));
    }
    try {
        const std::array parameters {
            foundation::Value {std::string(documentId.value())},
            foundation::Value {static_cast<std::int64_t>(sequence)}};
        auto rows = backend_->query(
            std::string("SELECT ") + std::string(selectColumns)
                + " FROM state_journal WHERE document_id=? AND sequence>? ORDER BY sequence",
            parameters);
        if(!rows) {
            return foundation::Result<std::vector<JournalRecord>>::failure(
                std::move(rows).error());
        }
        std::vector<JournalRecord> records;
        records.reserve(rows.value().size());
        std::uint64_t previous = sequence;
        for(const auto& row : rows.value()) {
            auto record = recordFromRow(row);
            if(!record) {
                return foundation::Result<std::vector<JournalRecord>>::failure(
                    std::move(record).error());
            }
            if(record.value().sequence <= previous || record.value().documentId != documentId) {
                return foundation::Result<std::vector<JournalRecord>>::failure(persistenceError(
                    "Persistence.JournalSequenceInvalid",
                    foundation::ErrorCategory::Infrastructure,
                    "Journal records are not in strict sequence order"));
            }
            auto valid = validateRecord(record.value(), *serializer_, *hashes_);
            if(!valid) {
                return foundation::Result<std::vector<JournalRecord>>::failure(
                    std::move(valid).error());
            }
            previous = record.value().sequence;
            records.push_back(std::move(record).value());
        }
        return foundation::Result<std::vector<JournalRecord>>::success(std::move(records));
    } catch(const std::exception& exception) {
        return foundation::Result<std::vector<JournalRecord>>::failure(persistenceError(
            "Persistence.JournalReadFailed",
            foundation::ErrorCategory::Internal,
            "The journal read failed unexpectedly",
            {{"reason", foundation::Value {exception.what()}}}));
    } catch(...) {
        return foundation::Result<std::vector<JournalRecord>>::failure(persistenceError(
            "Persistence.JournalReadFailed",
            foundation::ErrorCategory::Internal,
            "The journal read failed unexpectedly"));
    }
}

bool PersistenceService::configured() const
{
    std::lock_guard lock(mutex_);
    return backend_ != nullptr;
}

bool PersistenceService::ready() const
{
    std::lock_guard lock(mutex_);
    return initialized_;
}

bool PersistenceService::frozen() const
{
    std::lock_guard lock(mutex_);
    return frozen_;
}

void PersistenceService::freeze()
{
    std::lock_guard lock(mutex_);
    frozen_ = true;
}

} // namespace lasercnc::persistence
