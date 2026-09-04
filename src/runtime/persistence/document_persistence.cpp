#include <lasercnc/persistence/persistence_service.hpp>

#include <lasercnc/foundation/error.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace lasercnc::persistence {
namespace {

foundation::Error catalogError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    foundation::Value::Object details = {})
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {std::move(details)});
}

std::span<const std::byte> bytes(std::string_view value) noexcept
{
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

const char* stateName(DocumentPersistenceState state) noexcept
{
    switch(state) {
    case DocumentPersistenceState::Detached: return "detached";
    case DocumentPersistenceState::Opening: return "opening";
    case DocumentPersistenceState::Open: return "open";
    case DocumentPersistenceState::Closing: return "closing";
    case DocumentPersistenceState::Failed: return "failed";
    case DocumentPersistenceState::Removed: return "removed";
    }
    return "unknown";
}

bool isKnownState(DocumentPersistenceState state) noexcept
{
    switch(state) {
    case DocumentPersistenceState::Detached:
    case DocumentPersistenceState::Opening:
    case DocumentPersistenceState::Open:
    case DocumentPersistenceState::Closing:
    case DocumentPersistenceState::Failed:
    case DocumentPersistenceState::Removed:
        return true;
    }
    return false;
}

foundation::Result<DocumentPersistenceState> parseState(std::string_view state)
{
    if(state == "detached") {
        return foundation::Result<DocumentPersistenceState>::success(
            DocumentPersistenceState::Detached);
    }
    if(state == "opening") {
        return foundation::Result<DocumentPersistenceState>::success(
            DocumentPersistenceState::Opening);
    }
    if(state == "open") {
        return foundation::Result<DocumentPersistenceState>::success(
            DocumentPersistenceState::Open);
    }
    if(state == "closing") {
        return foundation::Result<DocumentPersistenceState>::success(
            DocumentPersistenceState::Closing);
    }
    if(state == "failed") {
        return foundation::Result<DocumentPersistenceState>::success(
            DocumentPersistenceState::Failed);
    }
    if(state == "removed") {
        return foundation::Result<DocumentPersistenceState>::success(
            DocumentPersistenceState::Removed);
    }
    return foundation::Result<DocumentPersistenceState>::failure(catalogError(
        "Persistence.InvalidDocumentLifecycleState",
        foundation::ErrorCategory::Infrastructure,
        "A persisted document lifecycle state is invalid",
        {{"state", foundation::Value {std::string(state)}}}));
}

foundation::Value catalogValue(
    const kernel::ProjectId& projectId,
    const kernel::DocumentId& documentId,
    DocumentPersistenceState state)
{
    return foundation::Value {foundation::Value::Object {
        {"documentId", foundation::Value {std::string(documentId.value())}},
        {"format", foundation::Value {"lasercnc.document-lifecycle.v1"}},
        {"projectId", foundation::Value {std::string(projectId.value())}},
        {"state", foundation::Value {stateName(state)}},
    }};
}

foundation::Result<std::string> textColumn(
    const platform::PersistenceRow& row,
    std::string_view name)
{
    const auto found = row.find(name);
    if(found == row.end()) {
        return foundation::Result<std::string>::failure(catalogError(
            "Persistence.DocumentCatalogColumnMissing",
            foundation::ErrorCategory::Infrastructure,
            "A document catalog row is missing a required column",
            {{"column", foundation::Value {std::string(name)}}}));
    }
    const auto* text = found->second.getIf<std::string>();
    if(text == nullptr) {
        return foundation::Result<std::string>::failure(catalogError(
            "Persistence.DocumentCatalogColumnTypeInvalid",
            foundation::ErrorCategory::Infrastructure,
            "A document catalog column has an invalid type",
            {{"column", foundation::Value {std::string(name)}}}));
    }
    return foundation::Result<std::string>::success(*text);
}

foundation::Result<std::int64_t> integerColumn(
    const platform::PersistenceRow& row,
    std::string_view name)
{
    const auto found = row.find(name);
    if(found == row.end()) {
        return foundation::Result<std::int64_t>::failure(catalogError(
            "Persistence.DocumentCatalogColumnMissing",
            foundation::ErrorCategory::Infrastructure,
            "A document catalog row is missing a required column",
            {{"column", foundation::Value {std::string(name)}}}));
    }
    const auto* value = found->second.getIf<std::int64_t>();
    if(value == nullptr) {
        return foundation::Result<std::int64_t>::failure(catalogError(
            "Persistence.DocumentCatalogColumnTypeInvalid",
            foundation::ErrorCategory::Infrastructure,
            "A document catalog column has an invalid type",
            {{"column", foundation::Value {std::string(name)}}}));
    }
    return foundation::Result<std::int64_t>::success(*value);
}

foundation::Result<void> rollback(
    platform::IPersistenceBackend& backend,
    foundation::Error error)
{
    auto rolledBack = backend.rollbackTransaction();
    if(rolledBack) {
        return foundation::Result<void>::failure(std::move(error));
    }
    return foundation::Result<void>::failure(catalogError(
        "Persistence.DocumentCatalogRollbackFailed",
        foundation::ErrorCategory::Infrastructure,
        "Document lifecycle persistence failed and could not roll back"));
}

} // namespace

foundation::Result<void> PersistenceService::saveDocumentLifecycle(
    const kernel::ProjectId& projectId,
    const kernel::DocumentId& documentId,
    DocumentPersistenceState state)
{
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<void>::failure(catalogError(
            "Persistence.NotReady",
            foundation::ErrorCategory::Conflict,
            "Persistence must be initialized before saving document lifecycle state"));
    }
    if(!isKnownState(state)) {
        return foundation::Result<void>::failure(catalogError(
            "Persistence.InvalidDocumentLifecycleState",
            foundation::ErrorCategory::Validation,
            "The document lifecycle state is invalid"));
    }
    bool transactionOpen = false;
    try {
        auto payload = serializer_->serialize(catalogValue(projectId, documentId, state));
        if(!payload) {
            return foundation::Result<void>::failure(std::move(payload).error());
        }
        auto digest = hashes_->digest(bytes(payload.value()));
        if(!digest) {
            return foundation::Result<void>::failure(std::move(digest).error());
        }
        auto begun = backend_->beginTransaction();
        if(!begun) {
            return begun;
        }
        transactionOpen = true;
        const std::array lookupParameters {
            foundation::Value {std::string(documentId.value())}};
        auto existing = backend_->query(
            "SELECT project_id,state,payload,digest FROM document_catalog "
            "WHERE document_id=?",
            lookupParameters);
        if(!existing) {
            return rollback(*backend_, std::move(existing).error());
        }
        if(existing.value().size() > 1U) {
            return rollback(*backend_, catalogError(
                "Persistence.DuplicateDocumentCatalogIdentity",
                foundation::ErrorCategory::Infrastructure,
                "The document catalog contains a duplicate stable identity"));
        }
        if(!existing.value().empty()) {
            auto storedProject = textColumn(existing.value().front(), "project_id");
            auto storedStateText = textColumn(existing.value().front(), "state");
            auto storedPayload = textColumn(existing.value().front(), "payload");
            auto storedDigestText = textColumn(existing.value().front(), "digest");
            if(!storedProject || !storedStateText || !storedPayload
               || !storedDigestText) {
                const auto error = !storedProject ? storedProject.error()
                    : !storedStateText ? storedStateText.error()
                    : !storedPayload ? storedPayload.error()
                                     : storedDigestText.error();
                return rollback(*backend_, error);
            }
            if(storedProject.value() != projectId.value()) {
                return rollback(*backend_, catalogError(
                    "Persistence.DocumentOwnershipConflict",
                    foundation::ErrorCategory::Conflict,
                    "A durable document identity is already owned by another project"));
            }
            auto storedState = parseState(storedStateText.value());
            auto storedDigest = kernel::ContentDigest::create(
                storedDigestText.value());
            auto actualDigest = hashes_->digest(bytes(storedPayload.value()));
            auto decoded = serializer_->deserialize(storedPayload.value());
            if(!storedState || !storedDigest || !actualDigest || !decoded) {
                return rollback(*backend_, catalogError(
                    "Persistence.InvalidDocumentCatalogRecord",
                    foundation::ErrorCategory::Infrastructure,
                    "The existing document lifecycle record is invalid"));
            }
            const auto expected = catalogValue(
                projectId, documentId, storedState.value());
            if(storedDigest.value() != actualDigest.value()
               || decoded.value() != expected) {
                return rollback(*backend_, catalogError(
                    "Persistence.DocumentCatalogIntegrityFailed",
                    foundation::ErrorCategory::Infrastructure,
                    "The existing document lifecycle record failed its integrity check"));
            }
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        const std::array parameters {
            foundation::Value {std::string(documentId.value())},
            foundation::Value {std::string(projectId.value())},
            foundation::Value {stateName(state)},
            foundation::Value {payload.value()},
            foundation::Value {std::string(digest.value().value())},
            foundation::Value {static_cast<std::int64_t>(now)},
        };
        auto saved = backend_->execute(
            "INSERT INTO document_catalog(document_id,project_id,state,payload,digest,updated_at_ms) "
            "VALUES(?,?,?,?,?,?) ON CONFLICT(document_id) DO UPDATE SET "
            "state=excluded.state,payload=excluded.payload,digest=excluded.digest,"
            "updated_at_ms=excluded.updated_at_ms",
            parameters);
        if(!saved) {
            return rollback(*backend_, std::move(saved).error());
        }
        auto committed = backend_->commitTransaction();
        if(!committed) {
            return rollback(*backend_, std::move(committed).error());
        }
        transactionOpen = false;
        return foundation::Result<void>::success();
    } catch(const std::exception& exception) {
        auto error = catalogError(
            "Persistence.DocumentCatalogWriteFailed",
            foundation::ErrorCategory::Internal,
            "Document lifecycle persistence failed unexpectedly",
            {{"reason", foundation::Value {exception.what()}}});
        return transactionOpen ? rollback(*backend_, std::move(error))
                               : foundation::Result<void>::failure(std::move(error));
    } catch(...) {
        auto error = catalogError(
            "Persistence.DocumentCatalogWriteFailed",
            foundation::ErrorCategory::Internal,
            "Document lifecycle persistence failed unexpectedly");
        return transactionOpen ? rollback(*backend_, std::move(error))
                               : foundation::Result<void>::failure(std::move(error));
    }
}

foundation::Result<void> PersistenceService::removeDocumentLifecycle(
    const kernel::ProjectId& projectId,
    const kernel::DocumentId& documentId)
{
    return saveDocumentLifecycle(
        projectId, documentId, DocumentPersistenceState::Removed);
}

foundation::Result<std::vector<DocumentCatalogRecord>>
PersistenceService::documentCatalog() const
{
    std::lock_guard lock(mutex_);
    return documentCatalogUnlocked();
}

foundation::Result<std::vector<DocumentCatalogRecord>>
PersistenceService::documentCatalogUnlocked(const std::optional<kernel::DocumentId>& filter) const
{
    if(!initialized_) {
        return foundation::Result<std::vector<DocumentCatalogRecord>>::failure(
            catalogError(
                "Persistence.NotReady",
                foundation::ErrorCategory::Conflict,
                "Persistence must be initialized before reading document lifecycle state"));
    }
    try {
    const std::string columns = "SELECT document_id,project_id,state,payload,digest,updated_at_ms FROM document_catalog";
    auto rows = filter
        ? backend_->query(columns + " WHERE document_id=?", std::array{foundation::Value{std::string(filter->value())}})
        : backend_->query(columns + " ORDER BY document_id");
    if(!rows) {
        return foundation::Result<std::vector<DocumentCatalogRecord>>::failure(
            std::move(rows).error());
    }
    std::vector<DocumentCatalogRecord> result;
    result.reserve(rows.value().size());
    for(const auto& row : rows.value()) {
        auto documentText = textColumn(row, "document_id");
        auto projectText = textColumn(row, "project_id");
        auto stateText = textColumn(row, "state");
        auto payload = textColumn(row, "payload");
        auto digestText = textColumn(row, "digest");
        auto updatedAt = integerColumn(row, "updated_at_ms");
        if(!documentText || !projectText || !stateText || !payload || !digestText
           || !updatedAt) {
            const auto* error = !documentText ? &documentText.error()
                : !projectText ? &projectText.error()
                : !stateText ? &stateText.error()
                : !payload ? &payload.error()
                : !digestText ? &digestText.error()
                              : &updatedAt.error();
            return foundation::Result<std::vector<DocumentCatalogRecord>>::failure(*error);
        }
        auto documentId = kernel::DocumentId::create(documentText.value());
        auto projectId = kernel::ProjectId::create(projectText.value());
        auto state = parseState(stateText.value());
        auto digest = kernel::ContentDigest::create(digestText.value());
        if(!documentId || !projectId || !state || !digest || updatedAt.value() < 0) {
            return foundation::Result<std::vector<DocumentCatalogRecord>>::failure(
                catalogError(
                    "Persistence.InvalidDocumentCatalogRecord",
                    foundation::ErrorCategory::Infrastructure,
                    "A persisted document lifecycle record is invalid"));
        }
        if(filter && (documentId.value() != *filter || rows.value().size() != 1U)) {
            return foundation::Result<std::vector<DocumentCatalogRecord>>::failure(catalogError(
                "Persistence.DocumentCatalogIntegrityFailed", foundation::ErrorCategory::Infrastructure,
                "A filtered document catalog lookup returned unexpected ownership"));
        }
        auto actualDigest = hashes_->digest(bytes(payload.value()));
        auto decoded = serializer_->deserialize(payload.value());
        if(!actualDigest || !decoded) {
            return foundation::Result<std::vector<DocumentCatalogRecord>>::failure(
                !actualDigest ? std::move(actualDigest).error()
                              : std::move(decoded).error());
        }
        const auto expected = catalogValue(
            projectId.value(), documentId.value(), state.value());
        if(actualDigest.value() != digest.value() || decoded.value() != expected) {
            return foundation::Result<std::vector<DocumentCatalogRecord>>::failure(
                catalogError(
                    "Persistence.DocumentCatalogIntegrityFailed",
                    foundation::ErrorCategory::Infrastructure,
                    "A persisted document lifecycle record failed its integrity check"));
        }
        const bool interrupted = state.value() == DocumentPersistenceState::Opening
            || state.value() == DocumentPersistenceState::Closing;
        result.push_back(DocumentCatalogRecord {
            std::move(projectId).value(),
            std::move(documentId).value(),
            interrupted ? DocumentPersistenceState::Failed : state.value(),
            interrupted,
            std::chrono::system_clock::time_point {
                std::chrono::milliseconds {updatedAt.value()}}});
    }
    return foundation::Result<std::vector<DocumentCatalogRecord>>::success(
        std::move(result));
    } catch(const std::exception& exception) {
        return foundation::Result<std::vector<DocumentCatalogRecord>>::failure(
            catalogError(
                "Persistence.DocumentCatalogReadFailed",
                foundation::ErrorCategory::Internal,
                "Document lifecycle recovery failed unexpectedly",
                {{"reason", foundation::Value {exception.what()}}}));
    } catch(...) {
        return foundation::Result<std::vector<DocumentCatalogRecord>>::failure(
            catalogError(
                "Persistence.DocumentCatalogReadFailed",
                foundation::ErrorCategory::Internal,
                "Document lifecycle recovery failed unexpectedly"));
    }
}

} // namespace lasercnc::persistence
