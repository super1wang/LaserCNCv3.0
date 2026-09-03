#include <lasercnc/state/document_store.hpp>

#include <lasercnc/foundation/error.hpp>

#include <mutex>
#include <string>
#include <utility>

namespace lasercnc::state {
namespace {

foundation::Error documentError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const kernel::DocumentId& id)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {foundation::Value::Object {
            {"documentId", foundation::Value {std::string(id.value())}},
        }});
}

} // namespace

foundation::Result<void> DocumentStore::addDocument(
    kernel::ProjectId projectId,
    kernel::DocumentId documentId)
{
    std::unique_lock lock(mutex_);
    if(documents_.contains(documentId)) {
        return foundation::Result<void>::failure(documentError(
            "Document.AlreadyExists",
            foundation::ErrorCategory::Conflict,
            "A document with the same stable ID already exists",
            documentId));
    }

    const auto [projectIterator, unused] = projectRevisions_.try_emplace(projectId, Revision {});
    static_cast<void>(unused);
    RevisionSet revisions;
    revisions.atMutable(RevisionScope::Project) = projectIterator->second;
    documents_.emplace(
        std::move(documentId), StoredDocument {std::move(projectId), std::move(revisions)});
    return foundation::Result<void>::success();
}

foundation::Result<Document> DocumentStore::snapshot(
    const kernel::DocumentId& documentId) const
{
    std::shared_lock lock(mutex_);
    const auto documentIterator = documents_.find(documentId);
    if(documentIterator == documents_.end()) {
        return foundation::Result<Document>::failure(documentError(
            "Document.NotFound",
            foundation::ErrorCategory::NotFound,
            "The document was not found",
            documentId));
    }

    auto revisions = documentIterator->second.revisions;
    const auto projectIterator = projectRevisions_.find(documentIterator->second.projectId);
    if(projectIterator == projectRevisions_.end()) {
        return foundation::Result<Document>::failure(documentError(
            "Document.ProjectRevisionMissing",
            foundation::ErrorCategory::Internal,
            "The document project revision is missing",
            documentId));
    }
    revisions.atMutable(RevisionScope::Project) = projectIterator->second;
    return foundation::Result<Document>::success(Document {
        documentIterator->second.projectId,
        documentIterator->first,
        std::move(revisions),
        documentIterator->second.objects});
}

bool DocumentStore::contains(const kernel::DocumentId& documentId) const
{
    std::shared_lock lock(mutex_);
    return documents_.contains(documentId);
}

std::size_t DocumentStore::size() const
{
    std::shared_lock lock(mutex_);
    return documents_.size();
}

} // namespace lasercnc::state
