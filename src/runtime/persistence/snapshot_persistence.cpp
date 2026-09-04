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
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

foundation::Error snapshotPersistenceError(
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

foundation::Value snapshotValue(
    const kernel::SnapshotId& snapshotId,
    const state::Document& document)
{
    foundation::Value::Array objects;
    const auto records = document.objects().all();
    objects.reserve(records.size());
    for(const auto& object : records) {
        objects.push_back(detail::encodeObjectRecord(object));
    }
    return foundation::Value {foundation::Value::Object {
        {"documentId", foundation::Value {std::string(document.id().value())}},
        {"format", foundation::Value {"lasercnc.document-snapshot"}},
        {"objects", foundation::Value {std::move(objects)}},
        {"projectId", foundation::Value {std::string(document.projectId().value())}},
        {"revisions", revisionsValue(document.revisions())},
        {"snapshotId", foundation::Value {std::string(snapshotId.value())}},
        {"version", foundation::Value {std::int64_t {3}}},
    }};
}

std::span<const std::byte> bytes(std::string_view value) noexcept
{
    return {
        reinterpret_cast<const std::byte*>(value.data()),
        value.size()};
}

foundation::Result<std::string> textColumn(
    const platform::PersistenceRow& row,
    const char* name)
{
    const auto found = row.find(name);
    const auto* text = found == row.end() ? nullptr : found->second.getIf<std::string>();
    if(text == nullptr) {
        return foundation::Result<std::string>::failure(snapshotPersistenceError(
            "Persistence.InvalidSnapshotRow",
            foundation::ErrorCategory::Infrastructure,
            "A snapshot index row contains a missing or invalid text column",
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
        return foundation::Result<std::int64_t>::failure(snapshotPersistenceError(
            "Persistence.InvalidSnapshotRow",
            foundation::ErrorCategory::Infrastructure,
            "A snapshot index row contains a missing or invalid integer column",
            {{"column", foundation::Value {name}}}));
    }
    return foundation::Result<std::int64_t>::success(*value);
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
        return foundation::Result<Id>::failure(snapshotPersistenceError(
            "Persistence.InvalidSnapshotRow",
            foundation::ErrorCategory::Infrastructure,
            "A snapshot index row contains an invalid stable identity",
            {{"column", foundation::Value {name}}},
            std::make_shared<const foundation::Error>(std::move(id).error())));
    }
    return id;
}

foundation::Result<state::Revision> parseRevision(std::string_view text)
{
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if(parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size()) {
        return foundation::Result<state::Revision>::failure(snapshotPersistenceError(
            "Persistence.InvalidSnapshotRow",
            foundation::ErrorCategory::Infrastructure,
            "A snapshot revision is invalid"));
    }
    return foundation::Result<state::Revision>::success(state::Revision {value});
}

foundation::Result<state::RevisionSet> revisionsFromRow(
    const platform::PersistenceRow& row,
    const char* suffix)
{
    std::array<state::Revision, revisionScopes.size()> values;
    for(std::size_t index = 0; index < revisionScopes.size(); ++index) {
        const auto column = std::string(revisionColumns[index]) + suffix;
        auto text = textColumn(row, column.c_str());
        if(!text) {
            return foundation::Result<state::RevisionSet>::failure(std::move(text).error());
        }
        auto revision = parseRevision(text.value());
        if(!revision) {
            return foundation::Result<state::RevisionSet>::failure(
                std::move(revision).error());
        }
        values[index] = revision.value();
    }
    return foundation::Result<state::RevisionSet>::success(state::RevisionSet {
        values[0], values[1], values[2], values[3], values[4], values[5]});
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
    return foundation::Result<void>::failure(snapshotPersistenceError(
        "Persistence.RollbackFailed",
        foundation::ErrorCategory::Infrastructure,
        "Snapshot persistence failed and its database transaction could not be rolled back",
        {{"rollbackCode", foundation::Value {std::string(rollbackError.code.value())}},
         {"rollbackMessage", foundation::Value {rollbackError.message}}},
        std::make_shared<const foundation::Error>(std::move(primary))));
}

foundation::Result<void> validateSnapshotPayload(
    const SnapshotRecord& record,
    const foundation::IValueSerializer& serializer,
    const platform::IHashService& hashes)
{
    auto digest = hashes.digest(bytes(record.payload));
    if(!digest) {
        return foundation::Result<void>::failure(std::move(digest).error());
    }
    if(digest.value() != record.digest) {
        return foundation::Result<void>::failure(snapshotPersistenceError(
            "Persistence.SnapshotDigestMismatch",
            foundation::ErrorCategory::Infrastructure,
            "A snapshot payload failed its content digest check",
            {{"snapshotId", foundation::Value {std::string(record.snapshotId.value())}}}));
    }
    auto decoded = serializer.deserialize(record.payload);
    if(!decoded) {
        return foundation::Result<void>::failure(snapshotPersistenceError(
            "Persistence.SnapshotPayloadInvalid",
            foundation::ErrorCategory::Infrastructure,
            "A snapshot payload cannot be decoded",
            {},
            std::make_shared<const foundation::Error>(std::move(decoded).error())));
    }
    const auto* root = decoded.value().getIf<foundation::Value::Object>();
    if(root == nullptr) {
        return foundation::Result<void>::failure(snapshotPersistenceError(
            "Persistence.SnapshotPayloadInvalid",
            foundation::ErrorCategory::Infrastructure,
            "A snapshot payload must be an object"));
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
    const auto revisions = root->find("revisions");
    const auto objects = root->find("objects");
    if(!matchesText("format", "lasercnc.document-snapshot")
       || versionNumber == nullptr || (*versionNumber != 1 && *versionNumber != 2 && *versionNumber != 3)
       || !matchesText("snapshotId", record.snapshotId.value())
       || !matchesText("projectId", record.projectId.value())
       || !matchesText("documentId", record.documentId.value())
       || revisions == root->end() || revisions->second != revisionsValue(record.revisions)
       || objects == root->end()
       || objects->second.kind() != foundation::Value::Kind::Array) {
        return foundation::Result<void>::failure(snapshotPersistenceError(
            "Persistence.SnapshotMetadataMismatch",
            foundation::ErrorCategory::Infrastructure,
            "A snapshot payload does not match its control-plane metadata",
            {{"snapshotId", foundation::Value {std::string(record.snapshotId.value())}}}));
    }
    return foundation::Result<void>::success();
}

constexpr std::string_view snapshotColumns =
    "snapshot_id,project_id,document_id,journal_sequence,"
    "project_revision,document_revision,geometry_revision,cam_revision,"
    "machine_context_revision,environment_revision,storage_key,digest,payload_size,"
    "created_at_ms";

foundation::Result<SnapshotRecord> snapshotFromRow(
    const platform::PersistenceRow& row,
    const platform::ISnapshotStore& store,
    const foundation::IValueSerializer& serializer,
    const platform::IHashService& hashes)
{
    auto snapshotId = idColumn<kernel::SnapshotId>(row, "snapshot_id");
    auto projectId = idColumn<kernel::ProjectId>(row, "project_id");
    auto documentId = idColumn<kernel::DocumentId>(row, "document_id");
    auto journalSequence = integerColumn(row, "journal_sequence");
    auto revisions = revisionsFromRow(row, "");
    auto storageKey = textColumn(row, "storage_key");
    auto digest = idColumn<kernel::ContentDigest>(row, "digest");
    auto payloadSize = integerColumn(row, "payload_size");
    auto createdAt = integerColumn(row, "created_at_ms");
    if(!snapshotId || !projectId || !documentId || !journalSequence || !revisions
       || !storageKey || !digest || !payloadSize || !createdAt
       || journalSequence.value() < 0 || payloadSize.value() < 0
       || createdAt.value() < 0) {
        return foundation::Result<SnapshotRecord>::failure(snapshotPersistenceError(
            "Persistence.InvalidSnapshotRow",
            foundation::ErrorCategory::Infrastructure,
            "A snapshot index row is invalid"));
    }
    if(storageKey.value() != snapshotId.value().value()) {
        return foundation::Result<SnapshotRecord>::failure(snapshotPersistenceError(
            "Persistence.SnapshotMetadataMismatch",
            foundation::ErrorCategory::Infrastructure,
            "A snapshot storage key does not match its stable identity"));
    }
    auto payload = store.read(snapshotId.value());
    if(!payload) {
        return foundation::Result<SnapshotRecord>::failure(std::move(payload).error());
    }
    if(payload.value().size() != static_cast<std::size_t>(payloadSize.value())) {
        return foundation::Result<SnapshotRecord>::failure(snapshotPersistenceError(
            "Persistence.SnapshotSizeMismatch",
            foundation::ErrorCategory::Infrastructure,
            "A snapshot payload size does not match its index"));
    }
    SnapshotRecord record {
        std::move(snapshotId).value(),
        std::move(projectId).value(),
        std::move(documentId).value(),
        std::move(revisions).value(),
        static_cast<std::uint64_t>(journalSequence.value()),
        std::move(payload).value(),
        std::move(digest).value(),
        std::chrono::system_clock::time_point {
            std::chrono::milliseconds {createdAt.value()}}};
    auto valid = validateSnapshotPayload(record, serializer, hashes);
    if(!valid) {
        return foundation::Result<SnapshotRecord>::failure(std::move(valid).error());
    }
    return foundation::Result<SnapshotRecord>::success(std::move(record));
}

bool localRevisionsEqual(
    const state::RevisionSet& left,
    const state::RevisionSet& right) noexcept
{
    for(const auto scope : revisionScopes) {
        if(scope != state::RevisionScope::Project && left.at(scope) != right.at(scope)) {
            return false;
        }
    }
    return true;
}

} // namespace

foundation::Result<SnapshotRecord> PersistenceService::captureSnapshot(
    kernel::SnapshotId snapshotId,
    const state::Document& document)
{
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<SnapshotRecord>::failure(snapshotPersistenceError(
            "Persistence.NotReady",
            foundation::ErrorCategory::Conflict,
            "Persistence must be initialized before capturing snapshots"));
    }
    if(snapshotStore_ == nullptr) {
        return foundation::Result<SnapshotRecord>::failure(snapshotPersistenceError(
            "Persistence.SnapshotStoreNotConfigured",
            foundation::ErrorCategory::Conflict,
            "A snapshot data store has not been configured"));
    }

    bool transactionOpen = false;
    try {
        auto payload = serializer_->serialize(snapshotValue(snapshotId, document));
        if(!payload) {
            return foundation::Result<SnapshotRecord>::failure(std::move(payload).error());
        }
        auto digest = hashes_->digest(bytes(payload.value()));
        if(!digest) {
            return foundation::Result<SnapshotRecord>::failure(std::move(digest).error());
        }
        if(payload.value().size()
           > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
            return foundation::Result<SnapshotRecord>::failure(snapshotPersistenceError(
                "Persistence.SnapshotPayloadTooLarge",
                foundation::ErrorCategory::Validation,
                "The snapshot payload exceeds the persistence metadata range"));
        }
        const auto createdAt = std::chrono::system_clock::now();
        const auto createdAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            createdAt.time_since_epoch()).count();
        if(createdAtMs < 0) {
            return foundation::Result<SnapshotRecord>::failure(snapshotPersistenceError(
                "Persistence.InvalidTimestamp",
                foundation::ErrorCategory::Internal,
                "The system clock produced an unsupported snapshot timestamp"));
        }

        auto begun = backend_->beginTransaction();
        if(!begun) {
            return foundation::Result<SnapshotRecord>::failure(std::move(begun).error());
        }
        transactionOpen = true;

        const std::array documentParameter {
            foundation::Value {std::string(document.id().value())}};
        auto journal = backend_->query(
            "SELECT sequence,project_id,"
            "project_revision_after,document_revision_after,geometry_revision_after,"
            "cam_revision_after,machine_context_revision_after,environment_revision_after "
            "FROM state_journal WHERE document_id=? ORDER BY sequence DESC LIMIT 1",
            documentParameter);
        if(!journal) {
            auto failure = rollback(*backend_, std::move(journal).error());
            return foundation::Result<SnapshotRecord>::failure(std::move(failure).error());
        }
        std::uint64_t journalSequence = 0U;
        if(journal.value().empty()) {
            state::RevisionSet localZero;
            if(!localRevisionsEqual(document.revisions(), localZero)) {
                auto failure = rollback(*backend_, snapshotPersistenceError(
                    "Persistence.SnapshotRevisionNotJournaled",
                    foundation::ErrorCategory::Conflict,
                    "A non-empty document revision has no matching journal record"));
                return foundation::Result<SnapshotRecord>::failure(
                    std::move(failure).error());
            }
        } else {
            if(journal.value().size() != 1U) {
                auto failure = rollback(*backend_, snapshotPersistenceError(
                    "Persistence.InvalidJournalRow",
                    foundation::ErrorCategory::Infrastructure,
                    "The latest journal lookup returned an invalid result"));
                return foundation::Result<SnapshotRecord>::failure(
                    std::move(failure).error());
            }
            auto sequence = integerColumn(journal.value().front(), "sequence");
            auto projectId = idColumn<kernel::ProjectId>(journal.value().front(), "project_id");
            auto revisions = revisionsFromRow(journal.value().front(), "_after");
            if(!sequence || !projectId || !revisions || sequence.value() <= 0
               || projectId.value() != document.projectId()
               || !localRevisionsEqual(revisions.value(), document.revisions())) {
                auto failure = rollback(*backend_, snapshotPersistenceError(
                    "Persistence.SnapshotRevisionNotJournaled",
                    foundation::ErrorCategory::Conflict,
                    "The document revision does not match the latest journal record"));
                return foundation::Result<SnapshotRecord>::failure(
                    std::move(failure).error());
            }
        }

        const std::array projectParameter {
            foundation::Value {std::string(document.projectId().value())}};
        auto projectJournal = backend_->query(
            "SELECT sequence,project_revision_after FROM state_journal "
            "WHERE project_id=? ORDER BY sequence DESC LIMIT 1",
            projectParameter);
        if(!projectJournal || projectJournal.value().size() > 1U) {
            auto failure = rollback(
                *backend_,
                projectJournal ? snapshotPersistenceError(
                    "Persistence.InvalidJournalRow",
                    foundation::ErrorCategory::Infrastructure,
                    "The project journal lookup returned an invalid result")
                               : std::move(projectJournal).error());
            return foundation::Result<SnapshotRecord>::failure(
                std::move(failure).error());
        }
        if(projectJournal.value().empty()) {
            if(document.revisions().at(state::RevisionScope::Project).value() != 0U) {
                auto failure = rollback(*backend_, snapshotPersistenceError(
                    "Persistence.SnapshotRevisionNotJournaled",
                    foundation::ErrorCategory::Conflict,
                    "The document project revision has no matching journal record"));
                return foundation::Result<SnapshotRecord>::failure(
                    std::move(failure).error());
            }
        } else {
            auto projectRevisionText = textColumn(
                projectJournal.value().front(), "project_revision_after");
            auto projectRevision = projectRevisionText
                ? parseRevision(projectRevisionText.value())
                : foundation::Result<state::Revision>::failure(
                    std::move(projectRevisionText).error());
            if(!projectRevision
               || projectRevision.value()
                   != document.revisions().at(state::RevisionScope::Project)) {
                auto failure = rollback(*backend_, snapshotPersistenceError(
                    "Persistence.SnapshotRevisionNotJournaled",
                    foundation::ErrorCategory::Conflict,
                    "The document project revision does not match the latest project journal record"));
                return foundation::Result<SnapshotRecord>::failure(
                    std::move(failure).error());
            }
        }

        auto globalJournal = backend_->query(
            "SELECT COALESCE(MAX(sequence),0) AS sequence FROM state_journal");
        if(!globalJournal || globalJournal.value().size() != 1U) {
            auto failure = rollback(
                *backend_,
                globalJournal ? snapshotPersistenceError(
                    "Persistence.InvalidJournalRow",
                    foundation::ErrorCategory::Infrastructure,
                    "The global journal sequence query returned an invalid result")
                              : std::move(globalJournal).error());
            return foundation::Result<SnapshotRecord>::failure(
                std::move(failure).error());
        }
        auto globalSequence = integerColumn(globalJournal.value().front(), "sequence");
        if(!globalSequence || globalSequence.value() < 0) {
            auto failure = rollback(*backend_, snapshotPersistenceError(
                "Persistence.InvalidJournalRow",
                foundation::ErrorCategory::Infrastructure,
                "The global journal sequence is invalid"));
            return foundation::Result<SnapshotRecord>::failure(
                std::move(failure).error());
        }
        journalSequence = static_cast<std::uint64_t>(globalSequence.value());

        const std::array snapshotParameter {
            foundation::Value {std::string(snapshotId.value())}};
        auto existing = backend_->query(
            std::string("SELECT ") + std::string(snapshotColumns)
                + " FROM snapshot_index WHERE snapshot_id=?",
            snapshotParameter);
        if(!existing) {
            auto failure = rollback(*backend_, std::move(existing).error());
            return foundation::Result<SnapshotRecord>::failure(std::move(failure).error());
        }
        if(!existing.value().empty()) {
            if(existing.value().size() != 1U) {
                auto failure = rollback(*backend_, snapshotPersistenceError(
                    "Persistence.InvalidSnapshotRow",
                    foundation::ErrorCategory::Infrastructure,
                    "A snapshot identity resolved ambiguously"));
                return foundation::Result<SnapshotRecord>::failure(
                    std::move(failure).error());
            }
            auto record = snapshotFromRow(
                existing.value().front(), *snapshotStore_, *serializer_, *hashes_);
            if(!record) {
                auto failure = rollback(*backend_, std::move(record).error());
                return foundation::Result<SnapshotRecord>::failure(
                    std::move(failure).error());
            }
            if(record.value().payload != payload.value()
               || record.value().digest != digest.value()
               || record.value().journalSequence != journalSequence
               || record.value().revisions != document.revisions()) {
                auto failure = rollback(*backend_, snapshotPersistenceError(
                    "Persistence.SnapshotIdentityConflict",
                    foundation::ErrorCategory::Conflict,
                    "A snapshot identity is already bound to different state"));
                return foundation::Result<SnapshotRecord>::failure(
                    std::move(failure).error());
            }
            auto committed = backend_->commitTransaction();
            if(!committed) {
                auto failure = rollback(*backend_, std::move(committed).error());
                return foundation::Result<SnapshotRecord>::failure(
                    std::move(failure).error());
            }
            transactionOpen = false;
            return record;
        }

        auto written = snapshotStore_->writeAtomically(snapshotId, payload.value());
        if(!written) {
            auto failure = rollback(*backend_, std::move(written).error());
            return foundation::Result<SnapshotRecord>::failure(std::move(failure).error());
        }
        std::vector<foundation::Value> parameters;
        parameters.reserve(14U);
        parameters.emplace_back(std::string(snapshotId.value()));
        parameters.emplace_back(std::string(document.projectId().value()));
        parameters.emplace_back(std::string(document.id().value()));
        parameters.emplace_back(static_cast<std::int64_t>(journalSequence));
        for(const auto scope : revisionScopes) {
            parameters.emplace_back(std::to_string(document.revisions().at(scope).value()));
        }
        parameters.emplace_back(std::string(snapshotId.value()));
        parameters.emplace_back(std::string(digest.value().value()));
        parameters.emplace_back(static_cast<std::int64_t>(payload.value().size()));
        parameters.emplace_back(static_cast<std::int64_t>(createdAtMs));
        auto inserted = backend_->execute(
            "INSERT INTO snapshot_index("
            "snapshot_id,project_id,document_id,journal_sequence,"
            "project_revision,document_revision,geometry_revision,cam_revision,"
            "machine_context_revision,environment_revision,storage_key,digest,payload_size,"
            "created_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            parameters);
        if(!inserted) {
            auto failure = rollback(*backend_, std::move(inserted).error());
            return foundation::Result<SnapshotRecord>::failure(std::move(failure).error());
        }
        auto committed = backend_->commitTransaction();
        if(!committed) {
            auto failure = rollback(*backend_, std::move(committed).error());
            return foundation::Result<SnapshotRecord>::failure(std::move(failure).error());
        }
        transactionOpen = false;
        return foundation::Result<SnapshotRecord>::success(SnapshotRecord {
            std::move(snapshotId),
            document.projectId(),
            document.id(),
            document.revisions(),
            journalSequence,
            std::move(payload).value(),
            std::move(digest).value(),
            createdAt});
    } catch(const std::exception& exception) {
        auto error = snapshotPersistenceError(
            "Persistence.SnapshotCaptureFailed",
            foundation::ErrorCategory::Internal,
            "Snapshot capture failed unexpectedly",
            {{"reason", foundation::Value {exception.what()}}});
        if(transactionOpen) {
            auto failure = rollback(*backend_, std::move(error));
            return foundation::Result<SnapshotRecord>::failure(std::move(failure).error());
        }
        return foundation::Result<SnapshotRecord>::failure(std::move(error));
    } catch(...) {
        auto error = snapshotPersistenceError(
            "Persistence.SnapshotCaptureFailed",
            foundation::ErrorCategory::Internal,
            "Snapshot capture failed unexpectedly");
        if(transactionOpen) {
            auto failure = rollback(*backend_, std::move(error));
            return foundation::Result<SnapshotRecord>::failure(std::move(failure).error());
        }
        return foundation::Result<SnapshotRecord>::failure(std::move(error));
    }
}

foundation::Result<std::optional<SnapshotRecord>> PersistenceService::latestSnapshot(
    const kernel::DocumentId& documentId) const
{
    std::lock_guard lock(mutex_);
    if(initialized_ && snapshotStore_ == nullptr) {
        return foundation::Result<std::optional<SnapshotRecord>>::failure(snapshotPersistenceError(
            "Persistence.SnapshotStoreNotConfigured", foundation::ErrorCategory::Conflict,
            "A snapshot data store has not been configured"));
    }
    return latestSnapshotUnlocked(documentId);
}

foundation::Result<std::optional<SnapshotRecord>> PersistenceService::latestSnapshotUnlocked(
    const kernel::DocumentId& documentId) const
{
    if(!initialized_) {
        return foundation::Result<std::optional<SnapshotRecord>>::failure(
            snapshotPersistenceError(
                "Persistence.NotReady",
                foundation::ErrorCategory::Conflict,
                "Persistence must be initialized before reading snapshots"));
    }
    try {
        const std::array parameters {
            foundation::Value {std::string(documentId.value())}};
        auto rows = backend_->query(
            std::string("SELECT ") + std::string(snapshotColumns)
                + " FROM snapshot_index WHERE document_id=? "
                  "ORDER BY journal_sequence DESC,created_at_ms DESC,snapshot_id DESC LIMIT 1",
            parameters);
        if(!rows) {
            return foundation::Result<std::optional<SnapshotRecord>>::failure(
                std::move(rows).error());
        }
        if(rows.value().empty()) {
            return foundation::Result<std::optional<SnapshotRecord>>::success(std::nullopt);
        }
        if(snapshotStore_ == nullptr) {
            return foundation::Result<std::optional<SnapshotRecord>>::failure(snapshotPersistenceError(
                "Persistence.SnapshotStoreNotConfigured", foundation::ErrorCategory::Conflict,
                "An indexed snapshot requires its data store for ownership verification"));
        }
        if(rows.value().size() != 1U) {
            return foundation::Result<std::optional<SnapshotRecord>>::failure(
                snapshotPersistenceError(
                    "Persistence.InvalidSnapshotRow",
                    foundation::ErrorCategory::Infrastructure,
                    "The latest snapshot lookup returned an invalid result"));
        }
        auto record = snapshotFromRow(
            rows.value().front(), *snapshotStore_, *serializer_, *hashes_);
        if(!record) {
            return foundation::Result<std::optional<SnapshotRecord>>::failure(
                std::move(record).error());
        }
        if(record.value().documentId != documentId) {
            return foundation::Result<std::optional<SnapshotRecord>>::failure(
                snapshotPersistenceError(
                    "Persistence.SnapshotMetadataMismatch",
                    foundation::ErrorCategory::Infrastructure,
                    "The latest snapshot belongs to a different document"));
        }
        return foundation::Result<std::optional<SnapshotRecord>>::success(
            std::move(record).value());
    } catch(const std::exception& exception) {
        return foundation::Result<std::optional<SnapshotRecord>>::failure(
            snapshotPersistenceError(
                "Persistence.SnapshotReadFailed",
                foundation::ErrorCategory::Internal,
                "Snapshot metadata read failed unexpectedly",
                {{"reason", foundation::Value {exception.what()}}}));
    } catch(...) {
        return foundation::Result<std::optional<SnapshotRecord>>::failure(
            snapshotPersistenceError(
                "Persistence.SnapshotReadFailed",
                foundation::ErrorCategory::Internal,
                "Snapshot metadata read failed unexpectedly"));
    }
}

} // namespace lasercnc::persistence
