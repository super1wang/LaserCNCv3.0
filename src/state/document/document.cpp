#include <lasercnc/state/document.hpp>

#include <utility>

namespace lasercnc::state {

Document::Document(
    kernel::ProjectId projectId,
    kernel::DocumentId documentId,
    RevisionSet revisions,
    ObjectRegistry objects)
    : projectId_(std::move(projectId)),
      documentId_(std::move(documentId)),
      revisions_(std::move(revisions)),
      objects_(std::move(objects))
{
}

const kernel::ProjectId& Document::projectId() const noexcept
{
    return projectId_;
}

const kernel::DocumentId& Document::id() const noexcept
{
    return documentId_;
}

const RevisionSet& Document::revisions() const noexcept
{
    return revisions_;
}

const ObjectRegistry& Document::objects() const noexcept
{
    return objects_;
}

} // namespace lasercnc::state
