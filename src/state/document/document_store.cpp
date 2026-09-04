#include <lasercnc/state/document_store.hpp>

#include <lasercnc/foundation/error.hpp>

#include <mutex>
#include <span>
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

foundation::Result<void> DocumentStore::restoreDocuments(
    std::span<const DocumentImage> images,
    const std::map<kernel::ProjectId, Revision>& recoveredProjectRevisions)
{
    std::unique_lock lock(mutex_);
    auto nextProjectRevisions = projectRevisions_;
    auto nextDocuments = documents_;
    // Project revisions survive even when none of their documents are loaded.
    // 中文翻译：即使项目没有任何已装载文档，也必须保留其已验证的项目修订。
    for(const auto& [projectId, revision] : recoveredProjectRevisions) {
        const auto existing = nextProjectRevisions.find(projectId);
        if(existing != nextProjectRevisions.end()
           && existing->second.value() != 0U && existing->second != revision) {
            return foundation::Result<void>::failure(foundation::makeError(
                "Document.RecoveryProjectRevisionConflict",
                foundation::ErrorCategory::Infrastructure,
                "A recovered project revision conflicts with installed state"));
        }
        nextProjectRevisions.insert_or_assign(projectId, revision);
    }
    for(const auto& image : images) {
        ObjectRegistry objects;
        for(const auto& object : image.objects) {
            const auto [unused, inserted] = objects.objects_.emplace(object.id, object);
            static_cast<void>(unused);
            if(!inserted) {
                return foundation::Result<void>::failure(documentError(
                    "Document.RecoveryDuplicateObject",
                    foundation::ErrorCategory::Infrastructure,
                    "A recovered document contains duplicate object identities",
                    image.documentId));
            }
        }

        const auto existing = nextDocuments.find(image.documentId);
        if(existing != nextDocuments.end()
           && existing->second.projectId != image.projectId) {
            return foundation::Result<void>::failure(documentError(
                "Document.RecoveryOwnershipConflict",
                foundation::ErrorCategory::Conflict,
                "A recovered document conflicts with its configured project ownership",
                image.documentId));
        }

        const auto project = nextProjectRevisions.find(image.projectId);
        if(project != nextProjectRevisions.end()
           && (project->second.value() != 0U || recoveredProjectRevisions.contains(image.projectId))
           && project->second != image.revisions.at(RevisionScope::Project)) {
            return foundation::Result<void>::failure(documentError(
                "Document.RecoveryProjectRevisionConflict",
                foundation::ErrorCategory::Infrastructure,
                "Recovered documents disagree on their project revision",
                image.documentId));
        }
        nextProjectRevisions[image.projectId] = image.revisions.at(RevisionScope::Project);
        auto revisions = image.revisions;
        nextDocuments.insert_or_assign(
            image.documentId,
            StoredDocument {image.projectId, std::move(revisions)});
        nextDocuments.at(image.documentId).objects.swap(objects);
    }

    for(auto& [unusedDocumentId, document] : nextDocuments) {
        static_cast<void>(unusedDocumentId);
        const auto project = nextProjectRevisions.find(document.projectId);
        if(project == nextProjectRevisions.end()) {
            return foundation::Result<void>::failure(documentError(
                "Document.ProjectRevisionMissing",
                foundation::ErrorCategory::Internal,
                "A recovered document project revision is missing",
                unusedDocumentId));
        }
        document.revisions.atMutable(RevisionScope::Project) = project->second;
    }
    projectRevisions_.swap(nextProjectRevisions);
    documents_.swap(nextDocuments);
    return foundation::Result<void>::success();
}

foundation::Result<void> DocumentStore::detachDocument(
    const kernel::DocumentId& documentId)
{
    std::unique_lock lock(mutex_);
    const auto found = documents_.find(documentId);
    if(found == documents_.end()) {
        return foundation::Result<void>::failure(documentError(
            "Document.NotFound",
            foundation::ErrorCategory::NotFound,
            "The document was not found",
            documentId));
    }
    documents_.erase(found);
    return foundation::Result<void>::success();
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
