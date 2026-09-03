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
    return foundation::Value {foundation::Value::Object {
        {"data", object.data},
        {"id", foundation::Value {std::string(object.id.value())}},
        {"type", foundation::Value {std::string(object.type.value())}},
    }};
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
        {"projectId", foundation::Value {std::string(commit.projectId.value())}},
        {"revisionsAfter", revisionsValue(commit.revisionsAfter)},
        {"revisionsBefore", revisionsValue(commit.revisionsBefore)},
        {"transactionId", foundation::Value {std::string(commit.transactionId.value())}},
        {"version", foundation::Value {std::int64_t {1}}},
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
    if(!matchesText("format", "lasercnc.state-journal")
       || versionNumber == nullptr || *versionNumber != 1
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

} // namespace

foundation::Result<void> PersistenceService::configure(
    std::unique_ptr<platform::IPersistenceBackend> backend,
    std::shared_ptr<foundation::IValueSerializer> serializer,
    std::shared_ptr<platform::IHashService> hashes)
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
    backend_ = std::move(backend);
    serializer_ = std::move(serializer);
    hashes_ = std::move(hashes);
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
        if(version.value() > 1) {
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
        const std::array migrationParameters {foundation::Value {std::int64_t {1}}};
        auto recorded = backend_->execute(
            "INSERT OR IGNORE INTO schema_migrations(version,applied_at) "
            "VALUES(?,CURRENT_TIMESTAMP)",
            migrationParameters);
        if(!recorded) {
            return rollback(*backend_, std::move(recorded).error());
        }
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
    const runtime::TransactionCommit& commit)
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
