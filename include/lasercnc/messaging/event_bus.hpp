#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/value.hpp>
#include <lasercnc/foundation/version.hpp>
#include <lasercnc/kernel/identifiers.hpp>
#include <lasercnc/messaging/domain_event.hpp>
#include <lasercnc/state/revision.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lasercnc::messaging {

enum class EventKind : std::uint8_t {
    Domain,
    Notification,
    System
};

enum class DeliveryMode : std::uint8_t {
    Immediate,
    Queued
};

class TransientEvent final {
public:
    [[nodiscard]] static TransientEvent notification(
        kernel::EventName name,
        foundation::Version version,
        foundation::Value payload,
        std::optional<std::string> coalescingKey = std::nullopt);
    [[nodiscard]] static TransientEvent system(
        kernel::EventName name,
        foundation::Version version,
        foundation::Value payload);

    [[nodiscard]] EventKind kind() const noexcept;
    [[nodiscard]] const kernel::EventName& name() const noexcept;
    [[nodiscard]] const foundation::Version& version() const noexcept;
    [[nodiscard]] const foundation::Value& payload() const noexcept;
    [[nodiscard]] const std::optional<std::string>& coalescingKey() const noexcept;

private:
    TransientEvent(
        EventKind kind,
        kernel::EventName name,
        foundation::Version version,
        foundation::Value payload,
        std::optional<std::string> coalescingKey);

    EventKind kind_;
    kernel::EventName name_;
    foundation::Version version_;
    foundation::Value payload_;
    std::optional<std::string> coalescingKey_;
};

class EventEnvelope final {
public:
    [[nodiscard]] EventKind kind() const noexcept;
    [[nodiscard]] const kernel::EventName& name() const noexcept;
    [[nodiscard]] const foundation::Version& version() const noexcept;
    [[nodiscard]] const foundation::Value& payload() const noexcept;
    [[nodiscard]] const std::optional<kernel::ObjectId>& aggregateId() const noexcept;
    [[nodiscard]] const std::optional<kernel::TransactionId>& transactionId() const noexcept;
    [[nodiscard]] const std::optional<kernel::ProjectId>& projectId() const noexcept;
    [[nodiscard]] const std::optional<kernel::DocumentId>& documentId() const noexcept;
    [[nodiscard]] const std::optional<state::RevisionSet>& revisions() const noexcept;
    [[nodiscard]] const std::optional<std::size_t>& sequence() const noexcept;
    [[nodiscard]] const std::optional<kernel::CorrelationId>& correlationId() const noexcept;
    [[nodiscard]] const std::optional<kernel::TraceId>& traceId() const noexcept;
    [[nodiscard]] const std::optional<std::string>& coalescingKey() const noexcept;

private:
    friend class EventBus;

    EventEnvelope(
        EventKind kind,
        kernel::EventName name,
        foundation::Version version,
        foundation::Value payload,
        std::optional<kernel::ObjectId> aggregateId,
        std::optional<kernel::TransactionId> transactionId,
        std::optional<kernel::ProjectId> projectId,
        std::optional<kernel::DocumentId> documentId,
        std::optional<state::RevisionSet> revisions,
        std::optional<std::size_t> sequence,
        std::optional<kernel::CorrelationId> correlationId,
        std::optional<kernel::TraceId> traceId,
        std::optional<std::string> coalescingKey);

    EventKind kind_;
    kernel::EventName name_;
    foundation::Version version_;
    foundation::Value payload_;
    std::optional<kernel::ObjectId> aggregateId_;
    std::optional<kernel::TransactionId> transactionId_;
    std::optional<kernel::ProjectId> projectId_;
    std::optional<kernel::DocumentId> documentId_;
    std::optional<state::RevisionSet> revisions_;
    std::optional<std::size_t> sequence_;
    std::optional<kernel::CorrelationId> correlationId_;
    std::optional<kernel::TraceId> traceId_;
    std::optional<std::string> coalescingKey_;
};

struct EventFilter final {
    std::optional<EventKind> kind;
    std::optional<kernel::EventName> name;
};

struct EventDeliveryFailure final {
    kernel::SubscriptionId subscriptionId;
    foundation::Error error;
};

struct EventDeliveryReport final {
    std::size_t matched{0U};
    std::size_t delivered{0U};
    std::size_t queued{0U};
    std::size_t coalesced{0U};
    std::vector<EventDeliveryFailure> failures;
};

using EventCallback = std::function<void(const EventEnvelope&)>;

namespace detail {
struct EventBusCore;
}

class EventSubscription final {
public:
    ~EventSubscription();

    EventSubscription(const EventSubscription&) = delete;
    EventSubscription& operator=(const EventSubscription&) = delete;
    EventSubscription(EventSubscription&& other) noexcept;
    EventSubscription& operator=(EventSubscription&& other) noexcept;

    [[nodiscard]] const kernel::SubscriptionId& id() const noexcept;
    void cancel() noexcept;

private:
    friend class EventBus;

    EventSubscription(
        std::weak_ptr<detail::EventBusCore> core,
        kernel::SubscriptionId subscriptionId);

    std::weak_ptr<detail::EventBusCore> core_;
    kernel::SubscriptionId subscriptionId_;
};

class EventBus final {
public:
    EventBus();
    ~EventBus();

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    [[nodiscard]] foundation::Result<EventSubscription> subscribe(
        kernel::SubscriptionId subscriptionId,
        EventFilter filter,
        DeliveryMode mode,
        EventCallback callback);
    [[nodiscard]] foundation::Result<EventDeliveryReport> publish(
        const CommittedDomainEvent& event,
        kernel::CorrelationId correlationId,
        kernel::TraceId traceId);
    [[nodiscard]] foundation::Result<EventDeliveryReport> publish(
        const TransientEvent& event,
        std::optional<kernel::CorrelationId> correlationId = std::nullopt,
        std::optional<kernel::TraceId> traceId = std::nullopt);
    [[nodiscard]] EventDeliveryReport drainQueued(
        std::size_t maximumDeliveries = static_cast<std::size_t>(-1));
    [[nodiscard]] std::size_t subscriptionCount() const;
    [[nodiscard]] std::size_t queuedCount() const;

private:
    [[nodiscard]] foundation::Result<EventDeliveryReport> publishEnvelope(
        EventEnvelope envelope);

    std::shared_ptr<detail::EventBusCore> core_;
};

} // namespace lasercnc::messaging
