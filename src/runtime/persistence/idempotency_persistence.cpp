#include "object_record_codec.hpp"

#include <lasercnc/persistence/persistence_service.hpp>

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/messaging/domain_event.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
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

foundation::Error idempotencyError(
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
    return foundation::Result<void>::failure(idempotencyError(
        "Persistence.RollbackFailed",
        foundation::ErrorCategory::Infrastructure,
        "Idempotency persistence failed and could not roll back",
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
        return foundation::Result<std::string>::failure(idempotencyError(
            "Persistence.InvalidIdempotencyRow",
            foundation::ErrorCategory::Infrastructure,
            "An idempotency row contains a missing or invalid text column",
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
        return foundation::Result<std::int64_t>::failure(idempotencyError(
            "Persistence.InvalidIdempotencyRow",
            foundation::ErrorCategory::Infrastructure,
            "An idempotency row contains a missing or invalid integer column",
            {{"column", foundation::Value {name}}}));
    }
    return foundation::Result<std::int64_t>::success(*value);
}

template <typename Id>
foundation::Result<Id> idText(std::string text, const char* field)
{
    auto id = Id::create(std::move(text));
    if(!id) {
        return foundation::Result<Id>::failure(idempotencyError(
            "Persistence.InvalidIdempotencyIdentity",
            foundation::ErrorCategory::Infrastructure,
            "Idempotency material contains an invalid stable identity",
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
        return foundation::Result<std::string>::failure(idempotencyError(
            "Persistence.InvalidIdempotencyPayload",
            foundation::ErrorCategory::Infrastructure,
            "An idempotency payload contains a missing or invalid text field",
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

foundation::Result<state::Revision> parseRevision(std::string_view text)
{
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if(parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size()) {
        return foundation::Result<state::Revision>::failure(idempotencyError(
            "Persistence.InvalidIdempotencyPayload",
            foundation::ErrorCategory::Infrastructure,
            "An idempotency payload contains an invalid revision"));
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

foundation::Result<state::RevisionSet> revisionsFromValue(
    const foundation::Value& value)
{
    const auto* object = value.getIf<foundation::Value::Object>();
    if(object == nullptr) {
        return foundation::Result<state::RevisionSet>::failure(idempotencyError(
            "Persistence.InvalidIdempotencyPayload",
            foundation::ErrorCategory::Infrastructure,
            "A persisted revision set must be an object"));
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

foundation::Value revisionsValue(const state::RevisionSet& revisions)
{
    foundation::Value::Object object;
    for(const auto scope : revisionScopes) {
        object.emplace(
            state::revisionScopeName(scope),
            foundation::Value {std::to_string(revisions.at(scope).value())});
    }
    return foundation::Value {std::move(object)};
}

foundation::Value objectValue(const state::ObjectRecord& object)
{
    return detail::encodeObjectRecord(object);
}

foundation::Result<state::ObjectRecord> objectFromValue(
    const foundation::Value& value, detail::ObjectRecordFormat objectFormat)
{
    return detail::decodeObjectRecord(value, objectFormat);
}

foundation::Result<std::optional<state::ObjectRecord>> optionalObject(
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
    foundation::Value version;
    if(history.commandVersion.has_value()) {
        version = foundation::Value {foundation::Value::Object {
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
        {"commandVersion", std::move(version)},
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
            {"after", change.after.has_value() ? objectValue(*change.after)
                                                : foundation::Value {}},
            {"before", change.before.has_value() ? objectValue(*change.before)
                                                  : foundation::Value {}},
            {"kind", foundation::Value {changeKindName(change.kind)}},
            {"objectId", foundation::Value {std::string(change.objectId.value())}},
        });
    }
    foundation::Value::Array events;
    events.reserve(commit.events.size());
    for(const auto& event : commit.events) {
        events.emplace_back(foundation::Value::Object {
            {"aggregateId", event.aggregateId().has_value()
                ? foundation::Value {std::string(event.aggregateId()->value())}
                : foundation::Value {}},
            {"name", foundation::Value {std::string(event.name().value())}},
            {"payload", event.payload()},
            {"sequence", foundation::Value {
                static_cast<std::int64_t>(event.sequence())}},
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
        {"history", historyValue(commit.history)},
        {"projectId", foundation::Value {std::string(commit.projectId.value())}},
        {"revisionsAfter", revisionsValue(commit.revisionsAfter)},
        {"revisionsBefore", revisionsValue(commit.revisionsBefore)},
        {"transactionId", foundation::Value {std::string(commit.transactionId.value())}},
    }};
}

foundation::Value outcomeValue(
    const runtime::TransactionCommit& commit,
    const foundation::Value& result)
{
    return foundation::Value {foundation::Value::Object {
        {"commit", commitValue(commit)},
        {"format", foundation::Value {"lasercnc.command-outcome"}},
        {"result", result},
        {"version", foundation::Value {std::int64_t {3}}},
    }};
}

foundation::Value taskOutcomeValue(
    const runtime::TaskRequest& request,
    const foundation::Value& result)
{
    return foundation::Value {foundation::Value::Object {
        {"commit", foundation::Value {}},
        {"format", foundation::Value {"lasercnc.command-outcome"}},
        {"result", result},
        {"taskId", foundation::Value {std::string(request.taskId.value())}},
        {"version", foundation::Value {std::int64_t {3}}},
    }};
}

struct PersistedEvent final {
    kernel::EventName name;
    foundation::Version version;
    std::optional<kernel::ObjectId> aggregateId;
    foundation::Value payload;
    std::size_t sequence{0U};
};

struct DecodedCommit final {
    kernel::TransactionId transactionId;
    kernel::ProjectId projectId;
    kernel::DocumentId documentId;
    state::RevisionSet before;
    state::RevisionSet after;
    std::vector<runtime::ObjectChange> changes;
    std::vector<PersistedEvent> events;
    runtime::HistoryMutation history;
};

bool changesEqual(
    std::span<const runtime::ObjectChange> left,
    std::span<const runtime::ObjectChange> right)
{
    if(left.size() != right.size()) {
        return false;
    }
    for(std::size_t index = 0U; index < left.size(); ++index) {
        if(left[index].kind != right[index].kind
           || left[index].objectId != right[index].objectId
           || left[index].before != right[index].before
           || left[index].after != right[index].after) {
            return false;
        }
    }
    return true;
}

foundation::Result<runtime::HistoryMutation> decodeHistoryValue(
    const foundation::Value& value)
{
    const auto* object = value.getIf<foundation::Value::Object>();
    if(object == nullptr) {
        return foundation::Result<runtime::HistoryMutation>::failure(idempotencyError(
            "Persistence.InvalidIdempotencyHistory",
            foundation::ErrorCategory::Infrastructure,
            "A persisted command history mutation must be an object"));
    }
    auto kind = stringField(*object, "kind");
    const auto* commandValue = field(*object, "command");
    const auto* versionValue = field(*object, "commandVersion");
    const auto* targetValue = field(*object, "targetTransactionId");
    const auto* cursorValue = field(*object, "expectedCursor");
    if(object->size() != 5U || !kind || commandValue == nullptr || versionValue == nullptr
       || targetValue == nullptr || cursorValue == nullptr) {
        return foundation::Result<runtime::HistoryMutation>::failure(idempotencyError(
            "Persistence.InvalidIdempotencyHistory",
            foundation::ErrorCategory::Infrastructure,
            "A persisted command history mutation is incomplete"));
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
        return foundation::Result<runtime::HistoryMutation>::failure(idempotencyError(
            "Persistence.InvalidIdempotencyHistory",
            foundation::ErrorCategory::Infrastructure,
            "A persisted command history mutation kind is invalid"));
    }
    if(commandValue->kind() != foundation::Value::Kind::Null) {
        const auto* text = commandValue->getIf<std::string>();
        if(text == nullptr) {
            return foundation::Result<runtime::HistoryMutation>::failure(idempotencyError(
                "Persistence.InvalidIdempotencyHistory",
                foundation::ErrorCategory::Infrastructure,
                "A persisted command history identity is invalid"));
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
        constexpr std::array names {"major", "minor", "patch"};
        std::array<std::uint32_t, 3U> parts {};
        if(version == nullptr || version->size() != 3U) {
            return foundation::Result<runtime::HistoryMutation>::failure(idempotencyError(
                "Persistence.InvalidIdempotencyHistory",
                foundation::ErrorCategory::Infrastructure,
                "A persisted command history version is invalid"));
        }
        for(std::size_t index = 0U; index < names.size(); ++index) {
            const auto* partValue = field(*version, names[index]);
            const auto* part = partValue == nullptr
                ? nullptr
                : partValue->getIf<std::int64_t>();
            if(part == nullptr || *part < 0
               || static_cast<std::uint64_t>(*part)
                    > std::numeric_limits<std::uint32_t>::max()) {
                return foundation::Result<runtime::HistoryMutation>::failure(idempotencyError(
                    "Persistence.InvalidIdempotencyHistory",
                    foundation::ErrorCategory::Infrastructure,
                    "A persisted command history version part is invalid"));
            }
            parts[index] = static_cast<std::uint32_t>(*part);
        }
        mutation.commandVersion = foundation::Version {parts[0], parts[1], parts[2]};
    }
    if(targetValue->kind() != foundation::Value::Kind::Null) {
        const auto* text = targetValue->getIf<std::string>();
        if(text == nullptr) {
            return foundation::Result<runtime::HistoryMutation>::failure(idempotencyError(
                "Persistence.InvalidIdempotencyHistory",
                foundation::ErrorCategory::Infrastructure,
                "A persisted command history target is invalid"));
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
            return foundation::Result<runtime::HistoryMutation>::failure(idempotencyError(
                "Persistence.InvalidIdempotencyHistory",
                foundation::ErrorCategory::Infrastructure,
                "A persisted command history cursor is invalid"));
        }
        const auto parsed = std::from_chars(
            text->data(), text->data() + text->size(), cursor);
        if(parsed.ec != std::errc {} || parsed.ptr != text->data() + text->size()) {
            return foundation::Result<runtime::HistoryMutation>::failure(idempotencyError(
                "Persistence.InvalidIdempotencyHistory",
                foundation::ErrorCategory::Infrastructure,
                "A persisted command history cursor is invalid"));
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
        return foundation::Result<runtime::HistoryMutation>::failure(idempotencyError(
            "Persistence.InvalidIdempotencyHistory",
            foundation::ErrorCategory::Infrastructure,
            "A persisted command history mutation has inconsistent fields"));
    }
    return foundation::Result<runtime::HistoryMutation>::success(std::move(mutation));
}

foundation::Result<DecodedCommit> decodeCommitValue(const foundation::Value& value, detail::ObjectRecordFormat objectFormat)
{
    const auto* root = value.getIf<foundation::Value::Object>();
    if(root == nullptr) {
        return foundation::Result<DecodedCommit>::failure(idempotencyError(
            "Persistence.InvalidIdempotencyPayload",
            foundation::ErrorCategory::Infrastructure,
            "A persisted command commit must be an object"));
    }
    auto transactionId = idField<kernel::TransactionId>(*root, "transactionId");
    auto projectId = idField<kernel::ProjectId>(*root, "projectId");
    auto documentId = idField<kernel::DocumentId>(*root, "documentId");
    const auto* beforeValue = field(*root, "revisionsBefore");
    const auto* afterValue = field(*root, "revisionsAfter");
    const auto* changesValue = field(*root, "changes");
    const auto* eventsValue = field(*root, "events");
    const auto* historyValue = field(*root, "history");
    if(!transactionId || !projectId || !documentId || beforeValue == nullptr
       || afterValue == nullptr || changesValue == nullptr || eventsValue == nullptr
       || (objectFormat != detail::ObjectRecordFormat::Legacy && historyValue == nullptr)) {
        return foundation::Result<DecodedCommit>::failure(idempotencyError(
            "Persistence.InvalidIdempotencyPayload",
            foundation::ErrorCategory::Infrastructure,
            "A persisted command commit is incomplete"));
    }
    auto before = revisionsFromValue(*beforeValue);
    auto after = revisionsFromValue(*afterValue);
    const auto* changes = changesValue->getIf<foundation::Value::Array>();
    const auto* events = eventsValue->getIf<foundation::Value::Array>();
    if(!before || !after || changes == nullptr || events == nullptr) {
        return foundation::Result<DecodedCommit>::failure(idempotencyError(
            "Persistence.InvalidIdempotencyPayload",
            foundation::ErrorCategory::Infrastructure,
            "A persisted command commit has invalid collections"));
    }

    std::vector<runtime::ObjectChange> decodedChanges;
    decodedChanges.reserve(changes->size());
    for(const auto& changeValue : *changes) {
        const auto* change = changeValue.getIf<foundation::Value::Object>();
        if(change == nullptr) {
            return foundation::Result<DecodedCommit>::failure(idempotencyError(
                "Persistence.InvalidIdempotencyPayload",
                foundation::ErrorCategory::Infrastructure,
                "A persisted command change must be an object"));
        }
        auto kind = stringField(*change, "kind");
        auto objectId = idField<kernel::ObjectId>(*change, "objectId");
        const auto* beforeObject = field(*change, "before");
        const auto* afterObject = field(*change, "after");
        if(!kind || !objectId || beforeObject == nullptr || afterObject == nullptr) {
            return foundation::Result<DecodedCommit>::failure(idempotencyError(
                "Persistence.InvalidIdempotencyPayload",
                foundation::ErrorCategory::Infrastructure,
                "A persisted command change is incomplete"));
        }
        auto previous = optionalObject(*beforeObject, objectFormat);
        auto next = optionalObject(*afterObject, objectFormat);
        if(!previous || !next) {
            return foundation::Result<DecodedCommit>::failure(idempotencyError(
                "Persistence.InvalidIdempotencyPayload",
                foundation::ErrorCategory::Infrastructure,
                "A persisted command change has invalid object material"));
        }
        runtime::ObjectChangeKind decodedKind;
        if(kind.value() == "created") {
            decodedKind = runtime::ObjectChangeKind::Created;
        } else if(kind.value() == "updated") {
            decodedKind = runtime::ObjectChangeKind::Updated;
        } else if(kind.value() == "removed") {
            decodedKind = runtime::ObjectChangeKind::Removed;
        } else {
            return foundation::Result<DecodedCommit>::failure(idempotencyError(
                "Persistence.InvalidIdempotencyPayload",
                foundation::ErrorCategory::Infrastructure,
                "A persisted command change kind is invalid"));
        }
        const bool shapeValid =
            (decodedKind == runtime::ObjectChangeKind::Created
             && !previous.value().has_value() && next.value().has_value())
            || (decodedKind == runtime::ObjectChangeKind::Updated
                && previous.value().has_value() && next.value().has_value())
            || (decodedKind == runtime::ObjectChangeKind::Removed
                && previous.value().has_value() && !next.value().has_value());
        if(!shapeValid
           || (previous.value().has_value()
               && previous.value()->id != objectId.value())
           || (next.value().has_value() && next.value()->id != objectId.value())) {
            return foundation::Result<DecodedCommit>::failure(idempotencyError(
                "Persistence.InvalidIdempotencyPayload",
                foundation::ErrorCategory::Infrastructure,
                "A persisted command change is inconsistent"));
        }
        decodedChanges.push_back(runtime::ObjectChange {
            decodedKind,
            std::move(objectId).value(),
            std::move(previous).value(),
            std::move(next).value()});
    }

    std::vector<PersistedEvent> decodedEvents;
    decodedEvents.reserve(events->size());
    std::size_t expectedEventSequence = 0U;
    for(const auto& eventValue : *events) {
        const auto* event = eventValue.getIf<foundation::Value::Object>();
        if(event == nullptr) {
            return foundation::Result<DecodedCommit>::failure(idempotencyError(
                "Persistence.InvalidIdempotencyPayload",
                foundation::ErrorCategory::Infrastructure,
                "A persisted command event must be an object"));
        }
        auto name = idField<kernel::EventName>(*event, "name");
        const auto* aggregateValue = field(*event, "aggregateId");
        const auto* payload = field(*event, "payload");
        const auto* sequenceValue = field(*event, "sequence");
        const auto* sequence = sequenceValue == nullptr
            ? nullptr
            : sequenceValue->getIf<std::int64_t>();
        const auto* versionValue = field(*event, "version");
        const auto* version = versionValue == nullptr
            ? nullptr
            : versionValue->getIf<foundation::Value::Object>();
        if(!name || aggregateValue == nullptr || payload == nullptr || sequence == nullptr
           || version == nullptr || *sequence < 0
           || static_cast<std::uint64_t>(*sequence) != expectedEventSequence) {
            return foundation::Result<DecodedCommit>::failure(idempotencyError(
                "Persistence.InvalidIdempotencyPayload",
                foundation::ErrorCategory::Infrastructure,
                "A persisted command event is incomplete or out of order"));
        }
        std::optional<kernel::ObjectId> aggregateId;
        if(aggregateValue->kind() != foundation::Value::Kind::Null) {
            const auto* aggregateText = aggregateValue->getIf<std::string>();
            if(aggregateText == nullptr) {
                return foundation::Result<DecodedCommit>::failure(idempotencyError(
                    "Persistence.InvalidIdempotencyPayload",
                    foundation::ErrorCategory::Infrastructure,
                    "A persisted command aggregate identity is invalid"));
            }
            auto parsedAggregate = idText<kernel::ObjectId>(*aggregateText, "aggregateId");
            if(!parsedAggregate) {
                return foundation::Result<DecodedCommit>::failure(
                    std::move(parsedAggregate).error());
            }
            aggregateId = std::move(parsedAggregate).value();
        }
        const auto readVersionPart = [version](const char* part)
            -> foundation::Result<std::uint32_t> {
            const auto* value = field(*version, part);
            const auto* integer = value == nullptr
                ? nullptr
                : value->getIf<std::int64_t>();
            if(integer == nullptr || *integer < 0
               || static_cast<std::uint64_t>(*integer)
                   > std::numeric_limits<std::uint32_t>::max()) {
                return foundation::Result<std::uint32_t>::failure(idempotencyError(
                    "Persistence.InvalidIdempotencyPayload",
                    foundation::ErrorCategory::Infrastructure,
                    "A persisted command event version is invalid"));
            }
            return foundation::Result<std::uint32_t>::success(
                static_cast<std::uint32_t>(*integer));
        };
        auto major = readVersionPart("major");
        auto minor = readVersionPart("minor");
        auto patch = readVersionPart("patch");
        if(!major || !minor || !patch) {
            return foundation::Result<DecodedCommit>::failure(idempotencyError(
                "Persistence.InvalidIdempotencyPayload",
                foundation::ErrorCategory::Infrastructure,
                "A persisted command event version is invalid"));
        }
        decodedEvents.push_back(PersistedEvent {
            std::move(name).value(),
            foundation::Version {major.value(), minor.value(), patch.value()},
            std::move(aggregateId),
            *payload,
            expectedEventSequence});
        ++expectedEventSequence;
    }
    runtime::HistoryMutation decodedHistory;
    if(historyValue != nullptr) {
        auto history = decodeHistoryValue(*historyValue);
        if(!history) {
            return foundation::Result<DecodedCommit>::failure(
                std::move(history).error());
        }
        decodedHistory = std::move(history).value();
    }
    if(decodedHistory.kind == runtime::HistoryMutationKind::None
       && (before.value() != after.value() || !decodedChanges.empty()
           || !decodedEvents.empty())) {
        decodedHistory.kind = runtime::HistoryMutationKind::Barrier;
    }
    return foundation::Result<DecodedCommit>::success(DecodedCommit {
        std::move(transactionId).value(),
        std::move(projectId).value(),
        std::move(documentId).value(),
        std::move(before).value(),
        std::move(after).value(),
        std::move(decodedChanges),
        std::move(decodedEvents),
        std::move(decodedHistory)});
}

foundation::Result<void> validatePayloadDigest(
    std::string_view payload,
    const kernel::ContentDigest& expected,
    const platform::IHashService& hashes,
    const char* mismatchCode)
{
    auto actual = hashes.digest(bytes(payload));
    if(!actual) {
        return foundation::Result<void>::failure(std::move(actual).error());
    }
    if(actual.value() != expected) {
        return foundation::Result<void>::failure(idempotencyError(
            mismatchCode,
            foundation::ErrorCategory::Infrastructure,
            "Persisted idempotency material failed its content digest check"));
    }
    return foundation::Result<void>::success();
}

} // namespace

foundation::Result<runtime::TransactionCommit> PersistenceService::loadCommitUnlocked(
    const kernel::TransactionId& transactionId) const
{
    const std::array parameters {
        foundation::Value {std::string(transactionId.value())}};
    auto rows = backend_->query(
        std::string("SELECT ") + std::string(journalColumns)
            + " FROM state_journal WHERE transaction_id=?",
        parameters);
    if(!rows) {
        return foundation::Result<runtime::TransactionCommit>::failure(
            std::move(rows).error());
    }
    if(rows.value().size() != 1U) {
        return foundation::Result<runtime::TransactionCommit>::failure(idempotencyError(
            "Persistence.IdempotencyJournalMissing",
            foundation::ErrorCategory::Infrastructure,
            "A completed command idempotency record has no unique journal record"));
    }
    const auto& row = rows.value().front();
    auto rowTransactionId = idColumn<kernel::TransactionId>(row, "transaction_id");
    auto projectId = idColumn<kernel::ProjectId>(row, "project_id");
    auto documentId = idColumn<kernel::DocumentId>(row, "document_id");
    auto before = revisionsFromRow(row, "_before");
    auto after = revisionsFromRow(row, "_after");
    auto payload = textColumn(row, "payload");
    auto digest = idColumn<kernel::ContentDigest>(row, "digest");
    if(!rowTransactionId || !projectId || !documentId || !before || !after
       || !payload || !digest || rowTransactionId.value() != transactionId) {
        return foundation::Result<runtime::TransactionCommit>::failure(idempotencyError(
            "Persistence.InvalidIdempotencyJournal",
            foundation::ErrorCategory::Infrastructure,
            "A completed idempotency journal row is invalid"));
    }
    auto validDigest = validatePayloadDigest(
        payload.value(), digest.value(), *hashes_, "Persistence.JournalDigestMismatch");
    if(!validDigest) {
        return foundation::Result<runtime::TransactionCommit>::failure(
            std::move(validDigest).error());
    }
    auto decoded = serializer_->deserialize(payload.value());
    if(!decoded) {
        return foundation::Result<runtime::TransactionCommit>::failure(
            std::move(decoded).error());
    }
    const auto* root = decoded.value().getIf<foundation::Value::Object>();
    if(root == nullptr) {
        return foundation::Result<runtime::TransactionCommit>::failure(idempotencyError(
            "Persistence.InvalidIdempotencyJournal",
            foundation::ErrorCategory::Infrastructure,
            "A completed idempotency journal payload must be an object"));
    }
    auto format = stringField(*root, "format");
    const auto* versionValue = field(*root, "version");
    const auto* version = versionValue == nullptr
        ? nullptr
        : versionValue->getIf<std::int64_t>();
    if(!format || format.value() != "lasercnc.state-journal" || version == nullptr
       || (*version != 1 && *version != 2 && *version != 3 && *version != 4)
       || (*version >= 2 && field(*root, "history") == nullptr)) {
        return foundation::Result<runtime::TransactionCommit>::failure(idempotencyError(
            "Persistence.InvalidIdempotencyJournal",
            foundation::ErrorCategory::Infrastructure,
            "A completed idempotency journal format is invalid"));
    }
    auto commit = decodeCommitValue(decoded.value(), detail::journalObjectFormat(*version));
    if(!commit || commit.value().transactionId != transactionId
       || commit.value().projectId != projectId.value()
       || commit.value().documentId != documentId.value()
       || commit.value().before != before.value()
       || commit.value().after != after.value()) {
        return foundation::Result<runtime::TransactionCommit>::failure(idempotencyError(
            "Persistence.InvalidIdempotencyJournal",
            foundation::ErrorCategory::Infrastructure,
            "A completed idempotency journal payload does not match its index"));
    }
    std::vector<messaging::CommittedDomainEvent> events;
    events.reserve(commit.value().events.size());
    for(auto& event : commit.value().events) {
        events.push_back(messaging::CommittedDomainEvent(
            std::move(event.name),
            event.version,
            std::move(event.aggregateId),
            std::move(event.payload),
            transactionId,
            projectId.value(),
            documentId.value(),
            after.value(),
            event.sequence));
    }
    auto decodedCommit = std::move(commit).value();
    return foundation::Result<runtime::TransactionCommit>::success(
        runtime::TransactionCommit {
            transactionId,
            std::move(projectId).value(),
            std::move(documentId).value(),
            std::move(before).value(),
            std::move(after).value(),
            std::move(decodedCommit.changes),
            std::move(events),
            std::move(decodedCommit.history)});
}

foundation::Result<IdempotencyClaim> PersistenceService::claimCommand(
    const kernel::IdempotencyKey& key,
    const foundation::Value& signature)
{
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<IdempotencyClaim>::failure(idempotencyError(
            "Persistence.NotReady",
            foundation::ErrorCategory::Conflict,
            "Persistence must be initialized before claiming idempotency"));
    }
    bool transactionOpen = false;
    try {
        auto signaturePayload = serializer_->serialize(signature);
        if(!signaturePayload) {
            return foundation::Result<IdempotencyClaim>::failure(
                std::move(signaturePayload).error());
        }
        auto signatureDigest = hashes_->digest(bytes(signaturePayload.value()));
        if(!signatureDigest) {
            return foundation::Result<IdempotencyClaim>::failure(
                std::move(signatureDigest).error());
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if(now < 0) {
            return foundation::Result<IdempotencyClaim>::failure(idempotencyError(
                "Persistence.InvalidTimestamp",
                foundation::ErrorCategory::Internal,
                "The system clock produced an unsupported idempotency timestamp"));
        }
        auto begun = backend_->beginTransaction();
        if(!begun) {
            return foundation::Result<IdempotencyClaim>::failure(std::move(begun).error());
        }
        transactionOpen = true;
        const std::array parameters {foundation::Value {std::string(key.value())}};
        auto rows = backend_->query(
            "SELECT signature_payload,signature_digest,status,outcome_payload,"
            "outcome_digest,journal_transaction_id,task_id "
            "FROM command_idempotency WHERE idempotency_key=?",
            parameters);
        if(!rows) {
            auto failure = rollback(*backend_, std::move(rows).error());
            return foundation::Result<IdempotencyClaim>::failure(
                std::move(failure).error());
        }
        if(rows.value().empty()) {
            const std::array insertParameters {
                foundation::Value {std::string(key.value())},
                foundation::Value {signaturePayload.value()},
                foundation::Value {std::string(signatureDigest.value().value())},
                foundation::Value {now}};
            auto inserted = backend_->execute(
                "INSERT INTO command_idempotency("
                "idempotency_key,signature_payload,signature_digest,status,created_at_ms) "
                "VALUES(?,?,?,'pending',?)",
                insertParameters);
            if(!inserted) {
                auto failure = rollback(*backend_, std::move(inserted).error());
                return foundation::Result<IdempotencyClaim>::failure(
                    std::move(failure).error());
            }
            auto committed = backend_->commitTransaction();
            if(!committed) {
                auto failure = rollback(*backend_, std::move(committed).error());
                return foundation::Result<IdempotencyClaim>::failure(
                    std::move(failure).error());
            }
            transactionOpen = false;
            return foundation::Result<IdempotencyClaim>::success(IdempotencyClaim {});
        }
        if(rows.value().size() != 1U) {
            auto failure = rollback(*backend_, idempotencyError(
                "Persistence.InvalidIdempotencyRow",
                foundation::ErrorCategory::Infrastructure,
                "An idempotency key resolved ambiguously"));
            return foundation::Result<IdempotencyClaim>::failure(
                std::move(failure).error());
        }
        const auto& row = rows.value().front();
        auto storedSignature = textColumn(row, "signature_payload");
        auto storedDigest = idColumn<kernel::ContentDigest>(row, "signature_digest");
        auto status = textColumn(row, "status");
        if(!storedSignature || !storedDigest || !status) {
            auto failure = rollback(*backend_, idempotencyError(
                "Persistence.InvalidIdempotencyRow",
                foundation::ErrorCategory::Infrastructure,
                "An idempotency row is incomplete"));
            return foundation::Result<IdempotencyClaim>::failure(
                std::move(failure).error());
        }
        auto storedValid = validatePayloadDigest(
            storedSignature.value(),
            storedDigest.value(),
            *hashes_,
            "Persistence.IdempotencySignatureDigestMismatch");
        if(!storedValid) {
            auto failure = rollback(*backend_, std::move(storedValid).error());
            return foundation::Result<IdempotencyClaim>::failure(
                std::move(failure).error());
        }
        if(storedSignature.value() != signaturePayload.value()
           || storedDigest.value() != signatureDigest.value()) {
            auto failure = rollback(*backend_, idempotencyError(
                "Persistence.IdempotencyKeyConflict",
                foundation::ErrorCategory::Conflict,
                "The idempotency key is bound to a different durable request signature"));
            return foundation::Result<IdempotencyClaim>::failure(
                std::move(failure).error());
        }
        if(status.value() == "pending") {
            auto failure = rollback(*backend_, idempotencyError(
                "Persistence.IdempotencyInProgress",
                foundation::ErrorCategory::Conflict,
                "The durable idempotency request is already in progress"));
            return foundation::Result<IdempotencyClaim>::failure(
                std::move(failure).error());
        }
        if(status.value() == "abandoned") {
            const std::array updateParameters {
                foundation::Value {now}, foundation::Value {std::string(key.value())}};
            auto updated = backend_->execute(
                "UPDATE command_idempotency SET status='pending',created_at_ms=?,"
                "completed_at_ms=NULL WHERE idempotency_key=? AND status='abandoned'",
                updateParameters);
            if(!updated || updated.value() != 1U) {
                auto failure = rollback(
                    *backend_,
                    updated ? idempotencyError(
                        "Persistence.IdempotencyClaimLost",
                        foundation::ErrorCategory::Conflict,
                        "The abandoned idempotency claim changed concurrently")
                            : std::move(updated).error());
                return foundation::Result<IdempotencyClaim>::failure(
                    std::move(failure).error());
            }
            auto committed = backend_->commitTransaction();
            if(!committed) {
                auto failure = rollback(*backend_, std::move(committed).error());
                return foundation::Result<IdempotencyClaim>::failure(
                    std::move(failure).error());
            }
            transactionOpen = false;
            return foundation::Result<IdempotencyClaim>::success(IdempotencyClaim {});
        }
        if(status.value() != "completed") {
            auto failure = rollback(*backend_, idempotencyError(
                "Persistence.InvalidIdempotencyStatus",
                foundation::ErrorCategory::Infrastructure,
                "An idempotency row contains an unknown status"));
            return foundation::Result<IdempotencyClaim>::failure(
                std::move(failure).error());
        }
        auto outcomePayload = textColumn(row, "outcome_payload");
        auto outcomeDigest = idColumn<kernel::ContentDigest>(row, "outcome_digest");
        if(!outcomePayload || !outcomeDigest) {
            auto failure = rollback(*backend_, idempotencyError(
                "Persistence.InvalidIdempotencyRow",
                foundation::ErrorCategory::Infrastructure,
                "A completed idempotency row is incomplete"));
            return foundation::Result<IdempotencyClaim>::failure(
                std::move(failure).error());
        }
        auto outcomeValid = validatePayloadDigest(
            outcomePayload.value(),
            outcomeDigest.value(),
            *hashes_,
            "Persistence.IdempotencyOutcomeDigestMismatch");
        if(!outcomeValid) {
            auto failure = rollback(*backend_, std::move(outcomeValid).error());
            return foundation::Result<IdempotencyClaim>::failure(
                std::move(failure).error());
        }
        auto outcome = serializer_->deserialize(outcomePayload.value());
        if(!outcome) {
            auto failure = rollback(*backend_, std::move(outcome).error());
            return foundation::Result<IdempotencyClaim>::failure(
                std::move(failure).error());
        }
        const auto* outcomeRoot = outcome.value().getIf<foundation::Value::Object>();
        auto format = outcomeRoot == nullptr
            ? foundation::Result<std::string>::failure(idempotencyError(
                "Persistence.InvalidIdempotencyPayload",
                foundation::ErrorCategory::Infrastructure,
                "A completed idempotency outcome must be an object"))
            : stringField(*outcomeRoot, "format");
        const auto* versionValue = outcomeRoot == nullptr
            ? nullptr
            : field(*outcomeRoot, "version");
        const auto* version = versionValue == nullptr
            ? nullptr
            : versionValue->getIf<std::int64_t>();
        const auto* result = outcomeRoot == nullptr
            ? nullptr
            : field(*outcomeRoot, "result");
        const auto* commitValueField = outcomeRoot == nullptr
            ? nullptr
            : field(*outcomeRoot, "commit");
        if(!format || format.value() != "lasercnc.command-outcome" || version == nullptr
           || (*version != 1 && *version != 2 && *version != 3) || result == nullptr || commitValueField == nullptr) {
            auto failure = rollback(*backend_, idempotencyError(
                "Persistence.InvalidIdempotencyPayload",
                foundation::ErrorCategory::Infrastructure,
                "A completed idempotency outcome format is invalid"));
            return foundation::Result<IdempotencyClaim>::failure(
                std::move(failure).error());
        }
        std::optional<runtime::TransactionCommit> replayCommit;
        std::optional<kernel::TaskId> replayTaskId;
        if(commitValueField->kind() == foundation::Value::Kind::Object) {
            auto journalTransactionId = idColumn<kernel::TransactionId>(
                row, "journal_transaction_id");
            auto embeddedCommit = decodeCommitValue(*commitValueField, detail::snapshotObjectFormat(*version));
            auto durableCommit = journalTransactionId
                ? loadCommitUnlocked(journalTransactionId.value())
                : foundation::Result<runtime::TransactionCommit>::failure(
                    std::move(journalTransactionId).error());
            if(!embeddedCommit || !durableCommit || !journalTransactionId
               || embeddedCommit.value().transactionId != journalTransactionId.value()
               || embeddedCommit.value().transactionId != durableCommit.value().transactionId
               || embeddedCommit.value().projectId != durableCommit.value().projectId
               || embeddedCommit.value().documentId != durableCommit.value().documentId
               || embeddedCommit.value().before != durableCommit.value().revisionsBefore
                || embeddedCommit.value().after != durableCommit.value().revisionsAfter
                || embeddedCommit.value().history != durableCommit.value().history
                || !changesEqual(
                   embeddedCommit.value().changes,
                   durableCommit.value().changes)) {
                auto failure = rollback(*backend_, idempotencyError(
                    "Persistence.IdempotencyOutcomeMismatch",
                    foundation::ErrorCategory::Infrastructure,
                    "A completed idempotency outcome does not match its journal record"));
                return foundation::Result<IdempotencyClaim>::failure(
                    std::move(failure).error());
            }
            replayCommit = std::move(durableCommit).value();
        } else if(commitValueField->kind() == foundation::Value::Kind::Null) {
            auto taskId = idColumn<kernel::TaskId>(row, "task_id");
            const auto* outcomeTaskValue = field(*outcomeRoot, "taskId");
            const auto* outcomeTaskText = outcomeTaskValue == nullptr
                ? nullptr
                : outcomeTaskValue->getIf<std::string>();
            if(!taskId || outcomeTaskText == nullptr
               || *outcomeTaskText != taskId.value().value()) {
                auto failure = rollback(*backend_, idempotencyError(
                    "Persistence.IdempotencyOutcomeMismatch",
                    foundation::ErrorCategory::Infrastructure,
                    "A completed task idempotency outcome does not match its task record"));
                return foundation::Result<IdempotencyClaim>::failure(
                    std::move(failure).error());
            }
            replayTaskId = std::move(taskId).value();
        } else {
            auto failure = rollback(*backend_, idempotencyError(
                "Persistence.InvalidIdempotencyPayload",
                foundation::ErrorCategory::Infrastructure,
                "A completed idempotency outcome has an invalid commit field"));
            return foundation::Result<IdempotencyClaim>::failure(
                std::move(failure).error());
        }
        auto committed = backend_->commitTransaction();
        if(!committed) {
            auto failure = rollback(*backend_, std::move(committed).error());
            return foundation::Result<IdempotencyClaim>::failure(
                std::move(failure).error());
        }
        transactionOpen = false;
        return foundation::Result<IdempotencyClaim>::success(IdempotencyClaim {
            IdempotencyClaimDisposition::Replayed,
            IdempotencyReplay {*result, std::move(replayCommit), std::move(replayTaskId)}});
    } catch(const std::exception& exception) {
        auto error = idempotencyError(
            "Persistence.IdempotencyClaimFailed",
            foundation::ErrorCategory::Internal,
            "The durable idempotency claim failed unexpectedly",
            {{"reason", foundation::Value {exception.what()}}});
        if(transactionOpen) {
            auto failure = rollback(*backend_, std::move(error));
            return foundation::Result<IdempotencyClaim>::failure(
                std::move(failure).error());
        }
        return foundation::Result<IdempotencyClaim>::failure(std::move(error));
    } catch(...) {
        auto error = idempotencyError(
            "Persistence.IdempotencyClaimFailed",
            foundation::ErrorCategory::Internal,
            "The durable idempotency claim failed unexpectedly");
        if(transactionOpen) {
            auto failure = rollback(*backend_, std::move(error));
            return foundation::Result<IdempotencyClaim>::failure(
                std::move(failure).error());
        }
        return foundation::Result<IdempotencyClaim>::failure(std::move(error));
    }
}

foundation::Result<void> PersistenceService::releaseCommandClaim(
    const kernel::IdempotencyKey& key,
    const foundation::Value& signature)
{
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<void>::failure(idempotencyError(
            "Persistence.NotReady",
            foundation::ErrorCategory::Conflict,
            "Persistence must be initialized before releasing idempotency"));
    }
    auto signaturePayload = serializer_->serialize(signature);
    if(!signaturePayload) {
        return foundation::Result<void>::failure(std::move(signaturePayload).error());
    }
    auto signatureDigest = hashes_->digest(bytes(signaturePayload.value()));
    if(!signatureDigest) {
        return foundation::Result<void>::failure(std::move(signatureDigest).error());
    }
    const std::array parameters {
        foundation::Value {std::string(key.value())},
        foundation::Value {std::string(signatureDigest.value().value())}};
    auto removed = backend_->execute(
        "DELETE FROM command_idempotency WHERE idempotency_key=? "
        "AND signature_digest=? AND status IN ('pending','abandoned')",
        parameters);
    if(!removed) {
        return foundation::Result<void>::failure(std::move(removed).error());
    }
    return foundation::Result<void>::success();
}

foundation::Result<void> PersistenceService::completeCommandInOpenTransaction(
    const runtime::TransactionCommit& commit,
    const runtime::TransactionIdempotency& idempotency)
{
    auto signaturePayload = serializer_->serialize(idempotency.signature);
    if(!signaturePayload) {
        return foundation::Result<void>::failure(std::move(signaturePayload).error());
    }
    auto signatureDigest = hashes_->digest(bytes(signaturePayload.value()));
    if(!signatureDigest) {
        return foundation::Result<void>::failure(std::move(signatureDigest).error());
    }
    auto outcomePayload = serializer_->serialize(outcomeValue(commit, idempotency.result));
    if(!outcomePayload) {
        return foundation::Result<void>::failure(std::move(outcomePayload).error());
    }
    auto outcomeDigest = hashes_->digest(bytes(outcomePayload.value()));
    if(!outcomeDigest) {
        return foundation::Result<void>::failure(std::move(outcomeDigest).error());
    }
    const std::array queryParameters {
        foundation::Value {std::string(idempotency.key.value())}};
    auto rows = backend_->query(
        "SELECT signature_payload,signature_digest,status,outcome_payload,"
        "outcome_digest,journal_transaction_id FROM command_idempotency "
        "WHERE idempotency_key=?",
        queryParameters);
    if(!rows) {
        return foundation::Result<void>::failure(std::move(rows).error());
    }
    if(rows.value().size() != 1U) {
        return foundation::Result<void>::failure(idempotencyError(
            "Persistence.IdempotencyClaimMissing",
            foundation::ErrorCategory::Conflict,
            "A command transaction has no unique durable idempotency claim"));
    }
    const auto& row = rows.value().front();
    auto storedSignature = textColumn(row, "signature_payload");
    auto storedSignatureDigest = idColumn<kernel::ContentDigest>(row, "signature_digest");
    auto status = textColumn(row, "status");
    if(!storedSignature || !storedSignatureDigest || !status
       || storedSignature.value() != signaturePayload.value()
       || storedSignatureDigest.value() != signatureDigest.value()) {
        return foundation::Result<void>::failure(idempotencyError(
            "Persistence.IdempotencyKeyConflict",
            foundation::ErrorCategory::Conflict,
            "The command transaction idempotency signature changed before completion"));
    }
    if(status.value() == "completed") {
        auto storedOutcome = textColumn(row, "outcome_payload");
        auto storedOutcomeDigest = idColumn<kernel::ContentDigest>(row, "outcome_digest");
        auto storedTransaction = idColumn<kernel::TransactionId>(
            row, "journal_transaction_id");
        if(!storedOutcome || !storedOutcomeDigest || !storedTransaction
           || storedOutcome.value() != outcomePayload.value()
           || storedOutcomeDigest.value() != outcomeDigest.value()
           || storedTransaction.value() != commit.transactionId) {
            return foundation::Result<void>::failure(idempotencyError(
                "Persistence.IdempotencyOutcomeConflict",
                foundation::ErrorCategory::Conflict,
                "A completed idempotency record is bound to a different outcome"));
        }
        return foundation::Result<void>::success();
    }
    if(status.value() != "pending") {
        return foundation::Result<void>::failure(idempotencyError(
            "Persistence.IdempotencyClaimLost",
            foundation::ErrorCategory::Conflict,
            "The durable idempotency claim is not active at transaction commit"));
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if(now < 0) {
        return foundation::Result<void>::failure(idempotencyError(
            "Persistence.InvalidTimestamp",
            foundation::ErrorCategory::Internal,
            "The system clock produced an unsupported idempotency completion timestamp"));
    }
    const std::array updateParameters {
        foundation::Value {outcomePayload.value()},
        foundation::Value {std::string(outcomeDigest.value().value())},
        foundation::Value {std::string(commit.transactionId.value())},
        foundation::Value {now},
        foundation::Value {std::string(idempotency.key.value())}};
    auto updated = backend_->execute(
        "UPDATE command_idempotency SET status='completed',outcome_payload=?,"
        "outcome_digest=?,journal_transaction_id=?,completed_at_ms=? "
        "WHERE idempotency_key=? AND status='pending'",
        updateParameters);
    if(!updated || updated.value() != 1U) {
        return updated ? foundation::Result<void>::failure(idempotencyError(
                             "Persistence.IdempotencyClaimLost",
                             foundation::ErrorCategory::Conflict,
                             "The durable idempotency claim changed before completion"))
                       : foundation::Result<void>::failure(std::move(updated).error());
    }
    return foundation::Result<void>::success();
}

foundation::Result<void> PersistenceService::completeTaskCommandInOpenTransaction(
    const runtime::TaskRequest& request,
    const runtime::TransactionIdempotency& idempotency)
{
    auto signaturePayload = serializer_->serialize(idempotency.signature);
    if(!signaturePayload) {
        return foundation::Result<void>::failure(std::move(signaturePayload).error());
    }
    auto signatureDigest = hashes_->digest(bytes(signaturePayload.value()));
    if(!signatureDigest) {
        return foundation::Result<void>::failure(std::move(signatureDigest).error());
    }
    auto outcomePayload = serializer_->serialize(
        taskOutcomeValue(request, idempotency.result));
    if(!outcomePayload) {
        return foundation::Result<void>::failure(std::move(outcomePayload).error());
    }
    auto outcomeDigest = hashes_->digest(bytes(outcomePayload.value()));
    if(!outcomeDigest) {
        return foundation::Result<void>::failure(std::move(outcomeDigest).error());
    }
    const std::array queryParameters {
        foundation::Value {std::string(idempotency.key.value())}};
    auto rows = backend_->query(
        "SELECT signature_payload,signature_digest,status,outcome_payload,"
        "outcome_digest,task_id FROM command_idempotency WHERE idempotency_key=?",
        queryParameters);
    if(!rows || rows.value().size() != 1U) {
        return rows ? foundation::Result<void>::failure(idempotencyError(
                          "Persistence.IdempotencyClaimMissing",
                          foundation::ErrorCategory::Conflict,
                          "A task acceptance has no unique durable idempotency claim"))
                    : foundation::Result<void>::failure(std::move(rows).error());
    }
    const auto& row = rows.value().front();
    auto storedSignature = textColumn(row, "signature_payload");
    auto storedSignatureDigest = idColumn<kernel::ContentDigest>(row, "signature_digest");
    auto status = textColumn(row, "status");
    if(!storedSignature || !storedSignatureDigest || !status
       || storedSignature.value() != signaturePayload.value()
       || storedSignatureDigest.value() != signatureDigest.value()) {
        return foundation::Result<void>::failure(idempotencyError(
            "Persistence.IdempotencyKeyConflict",
            foundation::ErrorCategory::Conflict,
            "The task acceptance idempotency signature changed before completion"));
    }
    if(status.value() == "completed") {
        auto storedOutcome = textColumn(row, "outcome_payload");
        auto storedOutcomeDigest = idColumn<kernel::ContentDigest>(row, "outcome_digest");
        auto storedTaskId = idColumn<kernel::TaskId>(row, "task_id");
        if(!storedOutcome || !storedOutcomeDigest || !storedTaskId
           || storedOutcome.value() != outcomePayload.value()
           || storedOutcomeDigest.value() != outcomeDigest.value()
           || storedTaskId.value() != request.taskId) {
            return foundation::Result<void>::failure(idempotencyError(
                "Persistence.IdempotencyOutcomeConflict",
                foundation::ErrorCategory::Conflict,
                "A completed idempotency record is bound to a different task outcome"));
        }
        return foundation::Result<void>::success();
    }
    if(status.value() != "pending") {
        return foundation::Result<void>::failure(idempotencyError(
            "Persistence.IdempotencyClaimLost",
            foundation::ErrorCategory::Conflict,
            "The durable task idempotency claim is not active"));
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if(now < 0) {
        return foundation::Result<void>::failure(idempotencyError(
            "Persistence.InvalidTimestamp",
            foundation::ErrorCategory::Internal,
            "The system clock produced an unsupported task completion timestamp"));
    }
    const std::array updateParameters {
        foundation::Value {outcomePayload.value()},
        foundation::Value {std::string(outcomeDigest.value().value())},
        foundation::Value {std::string(request.taskId.value())},
        foundation::Value {now},
        foundation::Value {std::string(idempotency.key.value())}};
    auto updated = backend_->execute(
        "UPDATE command_idempotency SET status='completed',outcome_payload=?,"
        "outcome_digest=?,task_id=?,completed_at_ms=? "
        "WHERE idempotency_key=? AND status='pending'",
        updateParameters);
    if(!updated || updated.value() != 1U) {
        return updated ? foundation::Result<void>::failure(idempotencyError(
                             "Persistence.IdempotencyClaimLost",
                             foundation::ErrorCategory::Conflict,
                             "The durable task idempotency claim changed before completion"))
                       : foundation::Result<void>::failure(std::move(updated).error());
    }
    return foundation::Result<void>::success();
}

} // namespace lasercnc::persistence
