#include "object_record_codec.hpp"
#include "journal_revision_validation.hpp"

#include <lasercnc/persistence/persistence_service.hpp>

#include <lasercnc/foundation/error.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
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

constexpr std::string_view journalColumns =
    "sequence,transaction_id,project_id,document_id,"
    "project_revision_before,document_revision_before,geometry_revision_before,"
    "cam_revision_before,machine_context_revision_before,environment_revision_before,"
    "project_revision_after,document_revision_after,geometry_revision_after,"
    "cam_revision_after,machine_context_revision_after,environment_revision_after,"
    "payload,digest,committed_at_ms";

constexpr std::string_view snapshotColumns =
    "snapshot_id,project_id,document_id,journal_sequence,"
    "project_revision,document_revision,geometry_revision,cam_revision,"
    "machine_context_revision,environment_revision,storage_key,digest,payload_size,"
    "created_at_ms";

foundation::Error recoveryError(
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
    return foundation::Result<void>::failure(recoveryError(
        "Persistence.RollbackFailed",
        foundation::ErrorCategory::Infrastructure,
        "Recovery failed and its database transaction could not be rolled back",
        {{"rollbackCode", foundation::Value {std::string(rollbackError.code.value())}},
         {"rollbackMessage", foundation::Value {rollbackError.message}}},
        std::make_shared<const foundation::Error>(std::move(primary))));
}

foundation::Result<std::string> textColumn(
    const platform::PersistenceRow& row,
    const char* name)
{
    const auto found = row.find(name);
    const auto* value = found == row.end() ? nullptr : found->second.getIf<std::string>();
    if(value == nullptr) {
        return foundation::Result<std::string>::failure(recoveryError(
            "Persistence.InvalidRecoveryRow",
            foundation::ErrorCategory::Infrastructure,
            "A recovery row contains a missing or invalid text column",
            {{"column", foundation::Value {name}}}));
    }
    return foundation::Result<std::string>::success(*value);
}

foundation::Result<std::int64_t> integerColumn(
    const platform::PersistenceRow& row,
    const char* name)
{
    const auto found = row.find(name);
    const auto* value = found == row.end() ? nullptr : found->second.getIf<std::int64_t>();
    if(value == nullptr) {
        return foundation::Result<std::int64_t>::failure(recoveryError(
            "Persistence.InvalidRecoveryRow",
            foundation::ErrorCategory::Infrastructure,
            "A recovery row contains a missing or invalid integer column",
            {{"column", foundation::Value {name}}}));
    }
    return foundation::Result<std::int64_t>::success(*value);
}

template <typename Id>
foundation::Result<Id> idText(std::string text, const char* field)
{
    auto id = Id::create(std::move(text));
    if(!id) {
        return foundation::Result<Id>::failure(recoveryError(
            "Persistence.InvalidRecoveryIdentity",
            foundation::ErrorCategory::Infrastructure,
            "Recovery material contains an invalid stable identity",
            {{"field", foundation::Value {field}}},
            std::make_shared<const foundation::Error>(std::move(id).error())));
    }
    return id;
}

template <typename Id>
foundation::Result<Id> idColumn(const platform::PersistenceRow& row, const char* name)
{
    auto value = textColumn(row, name);
    if(!value) {
        return foundation::Result<Id>::failure(std::move(value).error());
    }
    return idText<Id>(std::move(value).value(), name);
}

foundation::Result<state::Revision> parseRevision(std::string_view text)
{
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if(parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size()) {
        return foundation::Result<state::Revision>::failure(recoveryError(
            "Persistence.InvalidRecoveryRevision",
            foundation::ErrorCategory::Infrastructure,
            "Recovery material contains an invalid revision"));
    }
    return foundation::Result<state::Revision>::success(state::Revision {value});
}

foundation::Result<state::RevisionSet> revisionsFromRow(
    const platform::PersistenceRow& row,
    const char* suffix)
{
    std::array<state::Revision, revisionScopes.size()> values;
    for(std::size_t index = 0U; index < revisionScopes.size(); ++index) {
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

const foundation::Value* field(
    const foundation::Value::Object& object,
    std::string_view name) noexcept
{
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &found->second;
}

foundation::Result<std::string> stringField(
    const foundation::Value::Object& object,
    const char* name)
{
    const auto* value = field(object, name);
    const auto* text = value == nullptr ? nullptr : value->getIf<std::string>();
    if(text == nullptr) {
        return foundation::Result<std::string>::failure(recoveryError(
            "Persistence.InvalidRecoveryPayload",
            foundation::ErrorCategory::Infrastructure,
            "A recovery payload contains a missing or invalid text field",
            {{"field", foundation::Value {name}}}));
    }
    return foundation::Result<std::string>::success(*text);
}

template <typename Id>
foundation::Result<Id> idField(
    const foundation::Value::Object& object,
    const char* name)
{
    auto value = stringField(object, name);
    if(!value) {
        return foundation::Result<Id>::failure(std::move(value).error());
    }
    return idText<Id>(std::move(value).value(), name);
}

foundation::Result<state::RevisionSet> revisionsFromValue(
    const foundation::Value& value)
{
    const auto* object = value.getIf<foundation::Value::Object>();
    if(object == nullptr) {
        return foundation::Result<state::RevisionSet>::failure(recoveryError(
            "Persistence.InvalidRecoveryPayload",
            foundation::ErrorCategory::Infrastructure,
            "A recovery revision set must be an object"));
    }
    std::array<state::Revision, revisionScopes.size()> values;
    for(std::size_t index = 0U; index < revisionScopes.size(); ++index) {
        auto text = stringField(*object, state::revisionScopeName(revisionScopes[index]).data());
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

foundation::Result<state::ObjectRecord> objectFromValue(
    const foundation::Value& value, detail::ObjectRecordFormat objectFormat)
{
    return detail::decodeObjectRecord(value, objectFormat);
}

foundation::Result<std::optional<state::ObjectRecord>> optionalObjectFromValue(
    const foundation::Value& value, detail::ObjectRecordFormat objectFormat)
{
    if(value.kind() == foundation::Value::Kind::Null) {
        return foundation::Result<std::optional<state::ObjectRecord>>::success(
            std::nullopt);
    }
    auto object = objectFromValue(value, objectFormat);
    if(!object) {
        return foundation::Result<std::optional<state::ObjectRecord>>::failure(
            std::move(object).error());
    }
    return foundation::Result<std::optional<state::ObjectRecord>>::success(
        std::move(object).value());
}

foundation::Result<runtime::HistoryMutation> historyFromValue(
    const foundation::Value& value)
{
    const auto* object = value.getIf<foundation::Value::Object>();
    if(object == nullptr) {
        return foundation::Result<runtime::HistoryMutation>::failure(recoveryError(
            "Persistence.JournalHistoryInvalid",
            foundation::ErrorCategory::Infrastructure,
            "A journal history mutation must be an object"));
    }
    auto kind = stringField(*object, "kind");
    const auto* commandValue = field(*object, "command");
    const auto* versionValue = field(*object, "commandVersion");
    const auto* targetValue = field(*object, "targetTransactionId");
    const auto* cursorValue = field(*object, "expectedCursor");
    if(object->size() != 5U || !kind || commandValue == nullptr || versionValue == nullptr
       || targetValue == nullptr || cursorValue == nullptr) {
        return foundation::Result<runtime::HistoryMutation>::failure(recoveryError(
            "Persistence.JournalHistoryInvalid",
            foundation::ErrorCategory::Infrastructure,
            "A journal history mutation is incomplete"));
    }

    runtime::HistoryMutation mutation;
    if(kind.value() == "none") {
        mutation.kind = runtime::HistoryMutationKind::None;
    } else if(kind.value() == "record") {
        mutation.kind = runtime::HistoryMutationKind::Record;
    } else if(kind.value() == "barrier") {
        mutation.kind = runtime::HistoryMutationKind::Barrier;
    } else if(kind.value() == "undo") {
        mutation.kind = runtime::HistoryMutationKind::Undo;
    } else if(kind.value() == "redo") {
        mutation.kind = runtime::HistoryMutationKind::Redo;
    } else {
        return foundation::Result<runtime::HistoryMutation>::failure(recoveryError(
            "Persistence.JournalHistoryInvalid",
            foundation::ErrorCategory::Infrastructure,
            "A journal history mutation contains an unknown kind"));
    }

    if(commandValue->kind() != foundation::Value::Kind::Null) {
        const auto* text = commandValue->getIf<std::string>();
        if(text == nullptr) {
            return foundation::Result<runtime::HistoryMutation>::failure(recoveryError(
                "Persistence.JournalHistoryInvalid",
                foundation::ErrorCategory::Infrastructure,
                "A journal history command identity is invalid"));
        }
        auto command = idText<kernel::CommandName>(*text, "history.command");
        if(!command) {
            return foundation::Result<runtime::HistoryMutation>::failure(
                std::move(command).error());
        }
        mutation.command = std::move(command).value();
    }
    if(versionValue->kind() != foundation::Value::Kind::Null) {
        const auto* version = versionValue->getIf<foundation::Value::Object>();
        if(version == nullptr || version->size() != 3U) {
            return foundation::Result<runtime::HistoryMutation>::failure(recoveryError(
                "Persistence.JournalHistoryInvalid",
                foundation::ErrorCategory::Infrastructure,
                "A journal history command version is invalid"));
        }
        std::array<std::uint32_t, 3U> parts {};
        constexpr std::array names {"major", "minor", "patch"};
        for(std::size_t index = 0U; index < names.size(); ++index) {
            const auto* partValue = field(*version, names[index]);
            const auto* part = partValue == nullptr
                ? nullptr
                : partValue->getIf<std::int64_t>();
            if(part == nullptr || *part < 0
               || static_cast<std::uint64_t>(*part)
                    > std::numeric_limits<std::uint32_t>::max()) {
                return foundation::Result<runtime::HistoryMutation>::failure(recoveryError(
                    "Persistence.JournalHistoryInvalid",
                    foundation::ErrorCategory::Infrastructure,
                    "A journal history command version part is invalid"));
            }
            parts[index] = static_cast<std::uint32_t>(*part);
        }
        mutation.commandVersion = foundation::Version {parts[0], parts[1], parts[2]};
    }
    if(targetValue->kind() != foundation::Value::Kind::Null) {
        const auto* text = targetValue->getIf<std::string>();
        if(text == nullptr) {
            return foundation::Result<runtime::HistoryMutation>::failure(recoveryError(
                "Persistence.JournalHistoryInvalid",
                foundation::ErrorCategory::Infrastructure,
                "A journal history target identity is invalid"));
        }
        auto target = idText<kernel::TransactionId>(*text, "history.targetTransactionId");
        if(!target) {
            return foundation::Result<runtime::HistoryMutation>::failure(
                std::move(target).error());
        }
        mutation.targetTransactionId = std::move(target).value();
    }
    if(cursorValue->kind() != foundation::Value::Kind::Null) {
        const auto* text = cursorValue->getIf<std::string>();
        std::uint64_t cursor = 0U;
        if(text == nullptr) {
            return foundation::Result<runtime::HistoryMutation>::failure(recoveryError(
                "Persistence.JournalHistoryInvalid",
                foundation::ErrorCategory::Infrastructure,
                "A journal history cursor is invalid"));
        }
        const auto parsed = std::from_chars(
            text->data(), text->data() + text->size(), cursor);
        if(parsed.ec != std::errc {} || parsed.ptr != text->data() + text->size()) {
            return foundation::Result<runtime::HistoryMutation>::failure(recoveryError(
                "Persistence.JournalHistoryInvalid",
                foundation::ErrorCategory::Infrastructure,
                "A journal history cursor is invalid"));
        }
        mutation.expectedCursor = cursor;
    }

    const bool recordShape = mutation.kind == runtime::HistoryMutationKind::Record
        && mutation.command.has_value() && mutation.commandVersion.has_value()
        && !mutation.targetTransactionId.has_value() && !mutation.expectedCursor.has_value();
    const bool emptyShape =
        (mutation.kind == runtime::HistoryMutationKind::None
         || mutation.kind == runtime::HistoryMutationKind::Barrier)
        && !mutation.command.has_value() && !mutation.commandVersion.has_value()
        && !mutation.targetTransactionId.has_value() && !mutation.expectedCursor.has_value();
    const bool cursorShape =
        (mutation.kind == runtime::HistoryMutationKind::Undo
         || mutation.kind == runtime::HistoryMutationKind::Redo)
        && !mutation.command.has_value() && !mutation.commandVersion.has_value()
        && mutation.targetTransactionId.has_value() && mutation.expectedCursor.has_value();
    if(!recordShape && !emptyShape && !cursorShape) {
        return foundation::Result<runtime::HistoryMutation>::failure(recoveryError(
            "Persistence.JournalHistoryInvalid",
            foundation::ErrorCategory::Infrastructure,
            "A journal history mutation has inconsistent fields"));
    }
    return foundation::Result<runtime::HistoryMutation>::success(std::move(mutation));
}

struct DecodedJournal final {
    JournalRecord record;
    std::vector<runtime::ObjectChange> changes;
    runtime::HistoryMutation history;
};

foundation::Result<JournalRecord> journalRecordFromRow(
    const platform::PersistenceRow& row)
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
        return foundation::Result<JournalRecord>::failure(recoveryError(
            "Persistence.InvalidRecoveryRow",
            foundation::ErrorCategory::Infrastructure,
            "A state journal row is invalid during recovery"));
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

foundation::Result<DecodedJournal> decodeJournal(
    const platform::PersistenceRow& row,
    const foundation::IValueSerializer& serializer,
    const platform::IHashService& hashes)
{
    auto record = journalRecordFromRow(row);
    if(!record) {
        return foundation::Result<DecodedJournal>::failure(std::move(record).error());
    }
    auto actualDigest = hashes.digest(bytes(record.value().payload));
    if(!actualDigest) {
        return foundation::Result<DecodedJournal>::failure(
            std::move(actualDigest).error());
    }
    if(actualDigest.value() != record.value().digest) {
        return foundation::Result<DecodedJournal>::failure(recoveryError(
            "Persistence.JournalDigestMismatch",
            foundation::ErrorCategory::Infrastructure,
            "A journal payload failed its digest check during recovery",
            {{"sequence", foundation::Value {std::to_string(record.value().sequence)}}}));
    }
    auto decoded = serializer.deserialize(record.value().payload);
    if(!decoded) {
        return foundation::Result<DecodedJournal>::failure(recoveryError(
            "Persistence.JournalPayloadInvalid",
            foundation::ErrorCategory::Infrastructure,
            "A journal payload cannot be decoded during recovery",
            {},
            std::make_shared<const foundation::Error>(std::move(decoded).error())));
    }
    const auto* root = decoded.value().getIf<foundation::Value::Object>();
    if(root == nullptr) {
        return foundation::Result<DecodedJournal>::failure(recoveryError(
            "Persistence.JournalPayloadInvalid",
            foundation::ErrorCategory::Infrastructure,
            "A journal payload must be an object during recovery"));
    }
    auto format = stringField(*root, "format");
    auto transactionId = idField<kernel::TransactionId>(*root, "transactionId");
    auto projectId = idField<kernel::ProjectId>(*root, "projectId");
    auto documentId = idField<kernel::DocumentId>(*root, "documentId");
    const auto* versionValue = field(*root, "version");
    const auto* version = versionValue == nullptr
        ? nullptr
        : versionValue->getIf<std::int64_t>();
    const auto* beforeValue = field(*root, "revisionsBefore");
    const auto* afterValue = field(*root, "revisionsAfter");
    const auto* changesValue = field(*root, "changes");
    const auto* eventsValue = field(*root, "events");
    const auto* historyValue = field(*root, "history");
    if(!format || !transactionId || !projectId || !documentId || version == nullptr
       || beforeValue == nullptr || afterValue == nullptr || changesValue == nullptr
       || eventsValue == nullptr
       || eventsValue->kind() != foundation::Value::Kind::Array) {
        return foundation::Result<DecodedJournal>::failure(recoveryError(
            "Persistence.JournalPayloadInvalid",
            foundation::ErrorCategory::Infrastructure,
            "A journal payload is incomplete during recovery"));
    }
    auto before = revisionsFromValue(*beforeValue);
    auto after = revisionsFromValue(*afterValue);
    const auto* changes = changesValue->getIf<foundation::Value::Array>();
    const auto* events = eventsValue->getIf<foundation::Value::Array>();
    if(!before || !after || changes == nullptr || format.value() != "lasercnc.state-journal"
       || (*version != 1 && *version != 2 && *version != 3 && *version != 4)
       || (*version >= 2 && historyValue == nullptr)
       || transactionId.value() != record.value().transactionId
       || projectId.value() != record.value().projectId
       || documentId.value() != record.value().documentId
       || before.value() != record.value().revisionsBefore
       || after.value() != record.value().revisionsAfter) {
        return foundation::Result<DecodedJournal>::failure(recoveryError(
            "Persistence.JournalMetadataMismatch",
            foundation::ErrorCategory::Infrastructure,
            "A journal payload does not match its control-plane metadata during recovery",
            {{"sequence", foundation::Value {std::to_string(record.value().sequence)}}}));
    }

    std::vector<runtime::ObjectChange> decodedChanges;
    decodedChanges.reserve(changes->size());
    for(const auto& changeValue : *changes) {
        const auto* change = changeValue.getIf<foundation::Value::Object>();
        if(change == nullptr) {
            return foundation::Result<DecodedJournal>::failure(recoveryError(
                "Persistence.JournalChangeInvalid",
                foundation::ErrorCategory::Infrastructure,
                "A journal change must be an object"));
        }
        auto kind = stringField(*change, "kind");
        auto objectId = idField<kernel::ObjectId>(*change, "objectId");
        const auto* beforeObject = field(*change, "before");
        const auto* afterObject = field(*change, "after");
        if(!kind || !objectId || beforeObject == nullptr || afterObject == nullptr) {
            return foundation::Result<DecodedJournal>::failure(recoveryError(
                "Persistence.JournalChangeInvalid",
                foundation::ErrorCategory::Infrastructure,
                "A journal change is incomplete"));
        }
        auto beforeRecord = optionalObjectFromValue(*beforeObject, detail::journalObjectFormat(*version));
        auto afterRecord = optionalObjectFromValue(*afterObject, detail::journalObjectFormat(*version));
        if(!beforeRecord || !afterRecord) {
            return foundation::Result<DecodedJournal>::failure(recoveryError(
                "Persistence.JournalChangeInvalid",
                foundation::ErrorCategory::Infrastructure,
                "A journal change contains an invalid object record"));
        }
        runtime::ObjectChangeKind decodedKind;
        if(kind.value() == "created") {
            decodedKind = runtime::ObjectChangeKind::Created;
        } else if(kind.value() == "updated") {
            decodedKind = runtime::ObjectChangeKind::Updated;
        } else if(kind.value() == "removed") {
            decodedKind = runtime::ObjectChangeKind::Removed;
        } else {
            return foundation::Result<DecodedJournal>::failure(recoveryError(
                "Persistence.JournalChangeInvalid",
                foundation::ErrorCategory::Infrastructure,
                "A journal change contains an unknown change kind"));
        }
        const bool validShape =
            (decodedKind == runtime::ObjectChangeKind::Created
             && !beforeRecord.value().has_value() && afterRecord.value().has_value())
            || (decodedKind == runtime::ObjectChangeKind::Updated
                && beforeRecord.value().has_value() && afterRecord.value().has_value())
            || (decodedKind == runtime::ObjectChangeKind::Removed
                && beforeRecord.value().has_value() && !afterRecord.value().has_value());
        if(!validShape
           || (beforeRecord.value().has_value()
               && beforeRecord.value()->id != objectId.value())
           || (afterRecord.value().has_value()
               && afterRecord.value()->id != objectId.value())
           || (decodedKind == runtime::ObjectChangeKind::Updated
               && beforeRecord.value()->type != afterRecord.value()->type)) {
            return foundation::Result<DecodedJournal>::failure(recoveryError(
                "Persistence.JournalChangeInvalid",
                foundation::ErrorCategory::Infrastructure,
                "A journal change has inconsistent before/after material"));
        }
        decodedChanges.push_back(runtime::ObjectChange {
            decodedKind,
            std::move(objectId).value(),
            std::move(beforeRecord).value(),
            std::move(afterRecord).value()});
    }
    runtime::HistoryMutation decodedHistory;
    if(historyValue != nullptr) {
        auto parsedHistory = historyFromValue(*historyValue);
        if(!parsedHistory) {
            return foundation::Result<DecodedJournal>::failure(
                std::move(parsedHistory).error());
        }
        decodedHistory = std::move(parsedHistory).value();
    }
    if(decodedHistory.kind == runtime::HistoryMutationKind::None
       && (before.value() != after.value() || !decodedChanges.empty()
           || !events->empty())) {
        decodedHistory.kind = runtime::HistoryMutationKind::Barrier;
    }
    return foundation::Result<DecodedJournal>::success(DecodedJournal {
        std::move(record).value(), std::move(decodedChanges), std::move(decodedHistory)});
}

struct SnapshotState final {
    SnapshotRecord record;
    std::map<kernel::ObjectId, state::ObjectRecord> objects;
};

foundation::Result<SnapshotState> decodeSnapshot(
    const platform::PersistenceRow& row,
    const platform::ISnapshotStore& store,
    const foundation::IValueSerializer& serializer,
    const platform::IHashService& hashes)
{
    auto snapshotId = idColumn<kernel::SnapshotId>(row, "snapshot_id");
    auto projectId = idColumn<kernel::ProjectId>(row, "project_id");
    auto documentId = idColumn<kernel::DocumentId>(row, "document_id");
    auto sequence = integerColumn(row, "journal_sequence");
    auto revisions = revisionsFromRow(row, "");
    auto storageKey = textColumn(row, "storage_key");
    auto digest = idColumn<kernel::ContentDigest>(row, "digest");
    auto payloadSize = integerColumn(row, "payload_size");
    auto createdAt = integerColumn(row, "created_at_ms");
    if(!snapshotId || !projectId || !documentId || !sequence || !revisions
       || !storageKey || !digest || !payloadSize || !createdAt || sequence.value() < 0
       || payloadSize.value() < 0 || createdAt.value() < 0
       || storageKey.value() != snapshotId.value().value()) {
        return foundation::Result<SnapshotState>::failure(recoveryError(
            "Persistence.InvalidSnapshotRow",
            foundation::ErrorCategory::Infrastructure,
            "A snapshot index row is invalid during recovery"));
    }
    auto payload = store.read(snapshotId.value());
    if(!payload) {
        return foundation::Result<SnapshotState>::failure(std::move(payload).error());
    }
    if(payload.value().size() != static_cast<std::size_t>(payloadSize.value())) {
        return foundation::Result<SnapshotState>::failure(recoveryError(
            "Persistence.SnapshotSizeMismatch",
            foundation::ErrorCategory::Infrastructure,
            "A snapshot payload size does not match its index during recovery"));
    }
    auto actualDigest = hashes.digest(bytes(payload.value()));
    if(!actualDigest) {
        return foundation::Result<SnapshotState>::failure(
            std::move(actualDigest).error());
    }
    if(actualDigest.value() != digest.value()) {
        return foundation::Result<SnapshotState>::failure(recoveryError(
            "Persistence.SnapshotDigestMismatch",
            foundation::ErrorCategory::Infrastructure,
            "A snapshot payload failed its digest check during recovery"));
    }
    auto decoded = serializer.deserialize(payload.value());
    if(!decoded) {
        return foundation::Result<SnapshotState>::failure(recoveryError(
            "Persistence.SnapshotPayloadInvalid",
            foundation::ErrorCategory::Infrastructure,
            "A snapshot payload cannot be decoded during recovery",
            {},
            std::make_shared<const foundation::Error>(std::move(decoded).error())));
    }
    const auto* root = decoded.value().getIf<foundation::Value::Object>();
    if(root == nullptr) {
        return foundation::Result<SnapshotState>::failure(recoveryError(
            "Persistence.SnapshotPayloadInvalid",
            foundation::ErrorCategory::Infrastructure,
            "A snapshot payload must be an object during recovery"));
    }
    auto format = stringField(*root, "format");
    auto payloadSnapshotId = idField<kernel::SnapshotId>(*root, "snapshotId");
    auto payloadProjectId = idField<kernel::ProjectId>(*root, "projectId");
    auto payloadDocumentId = idField<kernel::DocumentId>(*root, "documentId");
    const auto* versionValue = field(*root, "version");
    const auto* version = versionValue == nullptr
        ? nullptr
        : versionValue->getIf<std::int64_t>();
    const auto* revisionsValue = field(*root, "revisions");
    const auto* objectsValue = field(*root, "objects");
    if(!format || !payloadSnapshotId || !payloadProjectId || !payloadDocumentId
       || version == nullptr || revisionsValue == nullptr || objectsValue == nullptr) {
        return foundation::Result<SnapshotState>::failure(recoveryError(
            "Persistence.SnapshotPayloadInvalid",
            foundation::ErrorCategory::Infrastructure,
            "A snapshot payload is incomplete during recovery"));
    }
    auto payloadRevisions = revisionsFromValue(*revisionsValue);
    const auto* objects = objectsValue->getIf<foundation::Value::Array>();
    if(!payloadRevisions || objects == nullptr
       || format.value() != "lasercnc.document-snapshot" || (*version != 1 && *version != 2 && *version != 3)
       || payloadSnapshotId.value() != snapshotId.value()
       || payloadProjectId.value() != projectId.value()
       || payloadDocumentId.value() != documentId.value()
       || payloadRevisions.value() != revisions.value()) {
        return foundation::Result<SnapshotState>::failure(recoveryError(
            "Persistence.SnapshotMetadataMismatch",
            foundation::ErrorCategory::Infrastructure,
            "A snapshot payload does not match its control-plane metadata during recovery"));
    }
    std::map<kernel::ObjectId, state::ObjectRecord> decodedObjects;
    for(const auto& objectValue : *objects) {
        auto object = objectFromValue(objectValue, detail::snapshotObjectFormat(*version));
        if(!object) {
            return foundation::Result<SnapshotState>::failure(
                std::move(object).error());
        }
        const auto [unused, inserted] = decodedObjects.emplace(
            object.value().id, std::move(object).value());
        static_cast<void>(unused);
        if(!inserted) {
            return foundation::Result<SnapshotState>::failure(recoveryError(
                "Persistence.SnapshotDuplicateObject",
                foundation::ErrorCategory::Infrastructure,
                "A snapshot contains duplicate object identities"));
        }
    }
    SnapshotRecord record {
        std::move(snapshotId).value(),
        std::move(projectId).value(),
        std::move(documentId).value(),
        std::move(revisions).value(),
        static_cast<std::uint64_t>(sequence.value()),
        std::move(payload).value(),
        std::move(digest).value(),
        std::chrono::system_clock::time_point {
            std::chrono::milliseconds {createdAt.value()}}};
    return foundation::Result<SnapshotState>::success(SnapshotState {
        std::move(record), std::move(decodedObjects)});
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

foundation::Result<void> validateRevisionTransition(const DecodedJournal& journal)
{
    return detail::validateJournalRevisionTransition(journal.record.revisionsBefore,
        journal.record.revisionsAfter, !journal.changes.empty());
}

foundation::Result<void> applyChanges(
    const DecodedJournal& journal,
    std::map<kernel::ObjectId, state::ObjectRecord>& objects)
{
    for(const auto& change : journal.changes) {
        const auto current = objects.find(change.objectId);
        switch(change.kind) {
        case runtime::ObjectChangeKind::Created:
            if(current != objects.end()) {
                return foundation::Result<void>::failure(recoveryError(
                    "Persistence.ReplayObjectConflict",
                    foundation::ErrorCategory::Infrastructure,
                    "Journal replay attempted to create an existing object"));
            }
            objects.emplace(change.objectId, *change.after);
            break;
        case runtime::ObjectChangeKind::Updated:
            if(current == objects.end() || current->second != *change.before) {
                return foundation::Result<void>::failure(recoveryError(
                    "Persistence.ReplayBeforeMismatch",
                    foundation::ErrorCategory::Infrastructure,
                    "Journal replay before-state does not match the recovered object"));
            }
            current->second = *change.after;
            break;
        case runtime::ObjectChangeKind::Removed:
            if(current == objects.end() || current->second != *change.before) {
                return foundation::Result<void>::failure(recoveryError(
                    "Persistence.ReplayBeforeMismatch",
                    foundation::ErrorCategory::Infrastructure,
                    "Journal replay before-state does not match the recovered object"));
            }
            objects.erase(current);
            break;
        }
    }
    return foundation::Result<void>::success();
}

struct WorkingDocument final {
    kernel::ProjectId projectId;
    kernel::DocumentId documentId;
    state::RevisionSet revisions;
    std::map<kernel::ObjectId, state::ObjectRecord> objects;
    std::uint64_t snapshotSequence{0U};
};

foundation::Result<void> validateSnapshotAnchor(
    const SnapshotState& snapshot,
    std::span<const DecodedJournal> journals,
    std::uint64_t latestSequence)
{
    if(snapshot.record.journalSequence > latestSequence) {
        return foundation::Result<void>::failure(recoveryError(
            "Persistence.SnapshotSequenceAhead",
            foundation::ErrorCategory::Infrastructure,
            "A snapshot sequence is ahead of the durable journal"));
    }
    state::RevisionSet expected;
    std::optional<kernel::ProjectId> documentProject;
    for(const auto& journal : journals) {
        if(journal.record.sequence > snapshot.record.journalSequence) {
            break;
        }
        if(journal.record.projectId == snapshot.record.projectId) {
            const auto projectRevision = journal.record.revisionsAfter.at(
                state::RevisionScope::Project);
            auto values = expected;
            values = state::RevisionSet {
                projectRevision,
                values.at(state::RevisionScope::Document),
                values.at(state::RevisionScope::Geometry),
                values.at(state::RevisionScope::Cam),
                values.at(state::RevisionScope::MachineContext),
                values.at(state::RevisionScope::Environment)};
            expected = std::move(values);
        }
        if(journal.record.documentId == snapshot.record.documentId) {
            if(documentProject.has_value()
               && *documentProject != journal.record.projectId) {
                return foundation::Result<void>::failure(recoveryError(
                    "Persistence.DocumentOwnershipChanged",
                    foundation::ErrorCategory::Infrastructure,
                    "A document changes project ownership in its journal history"));
            }
            documentProject = journal.record.projectId;
            expected = state::RevisionSet {
                expected.at(state::RevisionScope::Project),
                journal.record.revisionsAfter.at(state::RevisionScope::Document),
                journal.record.revisionsAfter.at(state::RevisionScope::Geometry),
                journal.record.revisionsAfter.at(state::RevisionScope::Cam),
                journal.record.revisionsAfter.at(state::RevisionScope::MachineContext),
                journal.record.revisionsAfter.at(state::RevisionScope::Environment)};
        }
    }
    if((documentProject.has_value() && *documentProject != snapshot.record.projectId)
       || expected != snapshot.record.revisions) {
        return foundation::Result<void>::failure(recoveryError(
            "Persistence.SnapshotRevisionMismatch",
            foundation::ErrorCategory::Infrastructure,
            "A snapshot revision does not match its journal sequence anchor"));
    }
    return foundation::Result<void>::success();
}

} // namespace

foundation::Result<RecoveryReport> PersistenceService::recover() const
{
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<RecoveryReport>::failure(recoveryError(
            "Persistence.NotReady",
            foundation::ErrorCategory::Conflict,
            "Persistence must be initialized before recovery"));
    }
    bool transactionOpen = false;
    try {
        auto begun = backend_->beginTransaction();
        if(!begun) {
            return foundation::Result<RecoveryReport>::failure(std::move(begun).error());
        }
        transactionOpen = true;
        auto journalRows = backend_->query(
            std::string("SELECT ") + std::string(journalColumns)
                + " FROM state_journal ORDER BY sequence");
        if(!journalRows) {
            auto failure = rollback(*backend_, std::move(journalRows).error());
            return foundation::Result<RecoveryReport>::failure(
                std::move(failure).error());
        }
        auto snapshotRows = backend_->query(
            std::string("SELECT ") + std::string(snapshotColumns)
                + " FROM snapshot_index ORDER BY document_id,journal_sequence DESC,"
                  "created_at_ms DESC,snapshot_id DESC");
        if(!snapshotRows) {
            auto failure = rollback(*backend_, std::move(snapshotRows).error());
            return foundation::Result<RecoveryReport>::failure(
                std::move(failure).error());
        }
        auto committed = backend_->commitTransaction();
        if(!committed) {
            auto failure = rollback(*backend_, std::move(committed).error());
            return foundation::Result<RecoveryReport>::failure(
                std::move(failure).error());
        }
        transactionOpen = false;

        std::vector<DecodedJournal> journals;
        journals.reserve(journalRows.value().size());
        std::map<kernel::ProjectId, state::Revision> projectRevisions;
        std::map<kernel::DocumentId, std::pair<kernel::ProjectId, state::RevisionSet>>
            documentRevisions;
        std::uint64_t expectedSequence = 1U;
        for(const auto& row : journalRows.value()) {
            auto journal = decodeJournal(row, *serializer_, *hashes_);
            if(!journal) {
                return foundation::Result<RecoveryReport>::failure(
                    std::move(journal).error());
            }
            if(journal.value().record.sequence != expectedSequence) {
                return foundation::Result<RecoveryReport>::failure(recoveryError(
                    "Persistence.JournalSequenceGap",
                    foundation::ErrorCategory::Infrastructure,
                    "The durable journal contains a missing or reordered sequence",
                    {{"expected", foundation::Value {std::to_string(expectedSequence)}},
                     {"actual", foundation::Value {
                        std::to_string(journal.value().record.sequence)}}}));
            }
            ++expectedSequence;
            auto transition = validateRevisionTransition(journal.value());
            if(!transition) {
                return foundation::Result<RecoveryReport>::failure(
                    std::move(transition).error());
            }
            const auto [project, unusedProject] = projectRevisions.try_emplace(
                journal.value().record.projectId, state::Revision {});
            static_cast<void>(unusedProject);
            if(project->second
               != journal.value().record.revisionsBefore.at(
                   state::RevisionScope::Project)) {
                return foundation::Result<RecoveryReport>::failure(recoveryError(
                    "Persistence.ProjectRevisionChainBroken",
                    foundation::ErrorCategory::Infrastructure,
                    "The project revision chain is discontinuous during recovery"));
            }
            project->second = journal.value().record.revisionsAfter.at(
                state::RevisionScope::Project);

            const auto [document, inserted] = documentRevisions.emplace(
                journal.value().record.documentId,
                std::pair {journal.value().record.projectId, state::RevisionSet {}});
            if(!inserted && document->second.first != journal.value().record.projectId) {
                return foundation::Result<RecoveryReport>::failure(recoveryError(
                    "Persistence.DocumentOwnershipChanged",
                    foundation::ErrorCategory::Infrastructure,
                    "A document changes project ownership in its journal history"));
            }
            if(!localRevisionsEqual(
                   document->second.second,
                   journal.value().record.revisionsBefore)) {
                return foundation::Result<RecoveryReport>::failure(recoveryError(
                    "Persistence.DocumentRevisionChainBroken",
                    foundation::ErrorCategory::Infrastructure,
                    "The document revision chain is discontinuous during recovery"));
            }
            document->second.second = journal.value().record.revisionsAfter;
            journals.push_back(std::move(journal).value());
        }
        const auto latestSequence = journals.empty()
            ? 0U
            : journals.back().record.sequence;

        std::map<kernel::DocumentId, WorkingDocument> working;
        for(const auto& [documentId, projectAndRevision] : documentRevisions) {
            auto revisions = state::RevisionSet {
                state::Revision {},
                state::Revision {},
                state::Revision {},
                state::Revision {},
                state::Revision {},
                state::Revision {}};
            working.emplace(
                documentId,
                WorkingDocument {
                    projectAndRevision.first,
                    documentId,
                    std::move(revisions),
                    {},
                    0U});
        }

        std::optional<kernel::DocumentId> previousSnapshotDocument;
        for(const auto& row : snapshotRows.value()) {
            auto rowDocumentId = idColumn<kernel::DocumentId>(row, "document_id");
            if(!rowDocumentId) {
                return foundation::Result<RecoveryReport>::failure(
                    std::move(rowDocumentId).error());
            }
            if(previousSnapshotDocument.has_value()
               && *previousSnapshotDocument == rowDocumentId.value()) {
                continue;
            }
            previousSnapshotDocument = rowDocumentId.value();
            if(snapshotStore_ == nullptr) {
                return foundation::Result<RecoveryReport>::failure(recoveryError(
                    "Persistence.SnapshotStoreNotConfigured",
                    foundation::ErrorCategory::Conflict,
                    "Recovery found snapshot metadata without a configured snapshot store"));
            }
            auto snapshot = decodeSnapshot(
                row, *snapshotStore_, *serializer_, *hashes_);
            if(!snapshot) {
                return foundation::Result<RecoveryReport>::failure(
                    std::move(snapshot).error());
            }
            auto anchor = validateSnapshotAnchor(
                snapshot.value(), journals, latestSequence);
            if(!anchor) {
                return foundation::Result<RecoveryReport>::failure(
                    std::move(anchor).error());
            }
            const auto existing = working.find(snapshot.value().record.documentId);
            if(existing != working.end()
               && existing->second.projectId != snapshot.value().record.projectId) {
                return foundation::Result<RecoveryReport>::failure(recoveryError(
                    "Persistence.DocumentOwnershipChanged",
                    foundation::ErrorCategory::Infrastructure,
                    "Snapshot and journal disagree on document ownership"));
            }
            working.insert_or_assign(
                snapshot.value().record.documentId,
                WorkingDocument {
                    snapshot.value().record.projectId,
                    snapshot.value().record.documentId,
                    snapshot.value().record.revisions,
                    std::move(snapshot).value().objects,
                    snapshot.value().record.journalSequence});
        }

        std::size_t replayed = 0U;
        for(const auto& journal : journals) {
            const auto target = working.find(journal.record.documentId);
            if(target == working.end()) {
                return foundation::Result<RecoveryReport>::failure(recoveryError(
                    "Persistence.RecoveryDocumentMissing",
                    foundation::ErrorCategory::Internal,
                    "A journal record has no recovery document"));
            }
            for(auto& [unusedId, document] : working) {
                static_cast<void>(unusedId);
                if(document.projectId == journal.record.projectId
                   && document.snapshotSequence < journal.record.sequence) {
                    if(document.revisions.at(state::RevisionScope::Project)
                       != journal.record.revisionsBefore.at(
                           state::RevisionScope::Project)) {
                        return foundation::Result<RecoveryReport>::failure(recoveryError(
                            "Persistence.ProjectRevisionChainBroken",
                            foundation::ErrorCategory::Infrastructure,
                            "A recovered project revision does not match journal replay"));
                    }
                    auto revisions = document.revisions;
                    revisions = state::RevisionSet {
                        journal.record.revisionsAfter.at(state::RevisionScope::Project),
                        revisions.at(state::RevisionScope::Document),
                        revisions.at(state::RevisionScope::Geometry),
                        revisions.at(state::RevisionScope::Cam),
                        revisions.at(state::RevisionScope::MachineContext),
                        revisions.at(state::RevisionScope::Environment)};
                    document.revisions = std::move(revisions);
                }
            }
            if(target->second.snapshotSequence >= journal.record.sequence) {
                continue;
            }
            if(!localRevisionsEqual(
                   target->second.revisions,
                   journal.record.revisionsBefore)) {
                return foundation::Result<RecoveryReport>::failure(recoveryError(
                    "Persistence.DocumentRevisionChainBroken",
                    foundation::ErrorCategory::Infrastructure,
                    "A recovered document revision does not match journal replay"));
            }
            auto applied = applyChanges(journal, target->second.objects);
            if(!applied) {
                return foundation::Result<RecoveryReport>::failure(
                    std::move(applied).error());
            }
            target->second.revisions = journal.record.revisionsAfter;
            ++replayed;
        }

        RecoveryReport report;
        report.latestJournalSequence = latestSequence;
        report.journalRecordsReplayed = replayed;
        report.historyCommits.reserve(journals.size());
        for(const auto& journal : journals) {
            report.historyCommits.push_back(runtime::TransactionCommit {
                journal.record.transactionId,
                journal.record.projectId,
                journal.record.documentId,
                journal.record.revisionsBefore,
                journal.record.revisionsAfter,
                journal.changes,
                {},
                journal.history});
        }
        report.documents.reserve(working.size());
        for(auto& [unusedId, document] : working) {
            static_cast<void>(unusedId);
            std::vector<state::ObjectRecord> objects;
            objects.reserve(document.objects.size());
            for(auto& [unusedObjectId, object] : document.objects) {
                static_cast<void>(unusedObjectId);
                objects.push_back(std::move(object));
            }
            report.documents.push_back(state::DocumentImage {
                std::move(document.projectId),
                std::move(document.documentId),
                std::move(document.revisions),
                std::move(objects)});
        }
        return foundation::Result<RecoveryReport>::success(std::move(report));
    } catch(const std::exception& exception) {
        auto error = recoveryError(
            "Persistence.RecoveryFailed",
            foundation::ErrorCategory::Internal,
            "Crash recovery failed unexpectedly",
            {{"reason", foundation::Value {exception.what()}}});
        if(transactionOpen) {
            auto failure = rollback(*backend_, std::move(error));
            return foundation::Result<RecoveryReport>::failure(
                std::move(failure).error());
        }
        return foundation::Result<RecoveryReport>::failure(std::move(error));
    } catch(...) {
        auto error = recoveryError(
            "Persistence.RecoveryFailed",
            foundation::ErrorCategory::Internal,
            "Crash recovery failed unexpectedly");
        if(transactionOpen) {
            auto failure = rollback(*backend_, std::move(error));
            return foundation::Result<RecoveryReport>::failure(
                std::move(failure).error());
        }
        return foundation::Result<RecoveryReport>::failure(std::move(error));
    }
}

} // namespace lasercnc::persistence
