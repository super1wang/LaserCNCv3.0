#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/state/document.hpp>

#include <cstddef>
#include <map>
#include <span>
#include <shared_mutex>
#include <utility>

namespace lasercnc::runtime {
class DocumentRuntime;
class TransactionManager;
}

namespace lasercnc::kernel {
class AppKernel;
}

namespace lasercnc::state {

class DocumentStore final {
public:
    DocumentStore() = default;

    DocumentStore(const DocumentStore&) = delete;
    DocumentStore& operator=(const DocumentStore&) = delete;

    [[nodiscard]] foundation::Result<void> addDocument(
        kernel::ProjectId projectId,
        kernel::DocumentId documentId);
    [[nodiscard]] foundation::Result<Document> snapshot(
        const kernel::DocumentId& documentId) const;
    [[nodiscard]] bool contains(const kernel::DocumentId& documentId) const;
    [[nodiscard]] std::size_t size() const;

private:
    friend class kernel::AppKernel;
    friend class runtime::DocumentRuntime;
    friend class runtime::TransactionManager;

    [[nodiscard]] foundation::Result<void> restoreDocuments(
        std::span<const DocumentImage> images,
        const std::map<kernel::ProjectId, Revision>& recoveredProjectRevisions = {});
    [[nodiscard]] foundation::Result<void> detachDocument(
        const kernel::DocumentId& documentId);

    struct StoredDocument final {
        StoredDocument(kernel::ProjectId project, RevisionSet revisionSet)
            : projectId(std::move(project)), revisions(std::move(revisionSet))
        {
        }

        kernel::ProjectId projectId;
        RevisionSet revisions;
        ObjectRegistry objects;
    };

    mutable std::shared_mutex mutex_;
    std::map<kernel::ProjectId, Revision> projectRevisions_;
    std::map<kernel::DocumentId, StoredDocument> documents_;
};

} // namespace lasercnc::state
