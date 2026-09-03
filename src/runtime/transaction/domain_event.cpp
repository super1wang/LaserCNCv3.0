#include <lasercnc/messaging/domain_event.hpp>

#include <utility>

namespace lasercnc::messaging {

CommittedDomainEvent::CommittedDomainEvent(
    kernel::EventName name,
    foundation::Version version,
    std::optional<kernel::ObjectId> aggregateId,
    foundation::Value payload,
    kernel::TransactionId transactionId,
    kernel::ProjectId projectId,
    kernel::DocumentId documentId,
    state::RevisionSet revisions,
    std::size_t sequence)
    : name_(std::move(name)),
      version_(version),
      aggregateId_(std::move(aggregateId)),
      payload_(std::move(payload)),
      transactionId_(std::move(transactionId)),
      projectId_(std::move(projectId)),
      documentId_(std::move(documentId)),
      revisions_(std::move(revisions)),
      sequence_(sequence)
{
}

const kernel::EventName& CommittedDomainEvent::name() const noexcept
{
    return name_;
}

const foundation::Version& CommittedDomainEvent::version() const noexcept
{
    return version_;
}

const std::optional<kernel::ObjectId>& CommittedDomainEvent::aggregateId() const noexcept
{
    return aggregateId_;
}

const foundation::Value& CommittedDomainEvent::payload() const noexcept
{
    return payload_;
}

const kernel::TransactionId& CommittedDomainEvent::transactionId() const noexcept
{
    return transactionId_;
}

const kernel::ProjectId& CommittedDomainEvent::projectId() const noexcept
{
    return projectId_;
}

const kernel::DocumentId& CommittedDomainEvent::documentId() const noexcept
{
    return documentId_;
}

const state::RevisionSet& CommittedDomainEvent::revisions() const noexcept
{
    return revisions_;
}

std::size_t CommittedDomainEvent::sequence() const noexcept
{
    return sequence_;
}

} // namespace lasercnc::messaging
