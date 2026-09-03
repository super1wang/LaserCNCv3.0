#pragma once

#include <lasercnc/kernel/identifiers.hpp>
#include <lasercnc/state/object_registry.hpp>
#include <lasercnc/state/revision.hpp>

#include <vector>

namespace lasercnc::state {

struct DocumentImage final {
    kernel::ProjectId projectId;
    kernel::DocumentId documentId;
    RevisionSet revisions;
    std::vector<ObjectRecord> objects;
};

class DocumentStore;

class Document final {
public:
    [[nodiscard]] const kernel::ProjectId& projectId() const noexcept;
    [[nodiscard]] const kernel::DocumentId& id() const noexcept;
    [[nodiscard]] const RevisionSet& revisions() const noexcept;
    [[nodiscard]] const ObjectRegistry& objects() const noexcept;

private:
    friend class DocumentStore;

    Document(
        kernel::ProjectId projectId,
        kernel::DocumentId documentId,
        RevisionSet revisions,
        ObjectRegistry objects);

    kernel::ProjectId projectId_;
    kernel::DocumentId documentId_;
    RevisionSet revisions_;
    ObjectRegistry objects_;
};

} // namespace lasercnc::state
