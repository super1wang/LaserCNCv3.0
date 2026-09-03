#pragma once

#include <lasercnc/foundation/value.hpp>
#include <lasercnc/foundation/version.hpp>
#include <lasercnc/kernel/identifiers.hpp>
#include <lasercnc/state/revision.hpp>

#include <cstddef>
#include <optional>

namespace lasercnc::runtime {
class TransactionManager;
}

namespace lasercnc::persistence {
class PersistenceService;
}

namespace lasercnc::messaging {

struct PendingDomainEvent final {
    kernel::EventName name;
    foundation::Version version;
    std::optional<kernel::ObjectId> aggregateId;
    foundation::Value payload;
};

class CommittedDomainEvent final {
public:
    [[nodiscard]] const kernel::EventName& name() const noexcept;
    [[nodiscard]] const foundation::Version& version() const noexcept;
    [[nodiscard]] const std::optional<kernel::ObjectId>& aggregateId() const noexcept;
    [[nodiscard]] const foundation::Value& payload() const noexcept;
    [[nodiscard]] const kernel::TransactionId& transactionId() const noexcept;
    [[nodiscard]] const kernel::ProjectId& projectId() const noexcept;
    [[nodiscard]] const kernel::DocumentId& documentId() const noexcept;
    [[nodiscard]] const state::RevisionSet& revisions() const noexcept;
    [[nodiscard]] std::size_t sequence() const noexcept;

private:
    friend class persistence::PersistenceService;
    friend class runtime::TransactionManager;

    CommittedDomainEvent(
        kernel::EventName name,
        foundation::Version version,
        std::optional<kernel::ObjectId> aggregateId,
        foundation::Value payload,
        kernel::TransactionId transactionId,
        kernel::ProjectId projectId,
        kernel::DocumentId documentId,
        state::RevisionSet revisions,
        std::size_t sequence);

    kernel::EventName name_;
    foundation::Version version_;
    std::optional<kernel::ObjectId> aggregateId_;
    foundation::Value payload_;
    kernel::TransactionId transactionId_;
    kernel::ProjectId projectId_;
    kernel::DocumentId documentId_;
    state::RevisionSet revisions_;
    std::size_t sequence_;
};

} // namespace lasercnc::messaging
