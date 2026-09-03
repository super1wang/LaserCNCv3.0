#include <lasercnc/messaging/event_bus.hpp>

#include <lasercnc/foundation/error.hpp>

#include <algorithm>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace lasercnc::messaging {
namespace detail {

struct EventBusCore final {
    struct SubscriptionEntry final {
        EventFilter filter;
        DeliveryMode mode;
        EventCallback callback;
    };

    struct QueuedDelivery final {
        kernel::SubscriptionId subscriptionId;
        EventEnvelope envelope;
    };

    std::mutex mutex;
    std::map<kernel::SubscriptionId, SubscriptionEntry> subscriptions;
    std::deque<QueuedDelivery> queued;
    bool closed{false};
};

} // namespace detail
namespace {

bool matches(const EventFilter& filter, const EventEnvelope& envelope)
{
    return (!filter.kind.has_value() || *filter.kind == envelope.kind())
        && (!filter.name.has_value() || *filter.name == envelope.name());
}

foundation::Error eventError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const kernel::SubscriptionId& subscriptionId)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {foundation::Value::Object {
            {"subscriptionId", foundation::Value {std::string(subscriptionId.value())}},
        }});
}

EventDeliveryFailure callbackFailure(
    const kernel::SubscriptionId& subscriptionId,
    const char* reason)
{
    return EventDeliveryFailure {
        subscriptionId,
        foundation::makeError(
            "Event.SubscriberFailed",
            foundation::ErrorCategory::Internal,
            "An event subscriber raised an exception",
            foundation::Value {foundation::Value::Object {
                {"subscriptionId", foundation::Value {std::string(subscriptionId.value())}},
                {"reason", foundation::Value {reason}},
            }})};
}

} // namespace

TransientEvent TransientEvent::notification(
    kernel::EventName name,
    foundation::Version version,
    foundation::Value payload,
    std::optional<std::string> coalescingKey)
{
    return TransientEvent(
        EventKind::Notification,
        std::move(name),
        version,
        std::move(payload),
        std::move(coalescingKey));
}

TransientEvent TransientEvent::system(
    kernel::EventName name,
    foundation::Version version,
    foundation::Value payload)
{
    return TransientEvent(
        EventKind::System,
        std::move(name),
        version,
        std::move(payload),
        std::nullopt);
}

TransientEvent::TransientEvent(
    EventKind kind,
    kernel::EventName name,
    foundation::Version version,
    foundation::Value payload,
    std::optional<std::string> coalescingKey)
    : kind_(kind),
      name_(std::move(name)),
      version_(version),
      payload_(std::move(payload)),
      coalescingKey_(std::move(coalescingKey))
{
}

EventKind TransientEvent::kind() const noexcept { return kind_; }
const kernel::EventName& TransientEvent::name() const noexcept { return name_; }
const foundation::Version& TransientEvent::version() const noexcept { return version_; }
const foundation::Value& TransientEvent::payload() const noexcept { return payload_; }
const std::optional<std::string>& TransientEvent::coalescingKey() const noexcept
{
    return coalescingKey_;
}

EventEnvelope::EventEnvelope(
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
    std::optional<std::string> coalescingKey)
    : kind_(kind),
      name_(std::move(name)),
      version_(version),
      payload_(std::move(payload)),
      aggregateId_(std::move(aggregateId)),
      transactionId_(std::move(transactionId)),
      projectId_(std::move(projectId)),
      documentId_(std::move(documentId)),
      revisions_(std::move(revisions)),
      sequence_(sequence),
      correlationId_(std::move(correlationId)),
      traceId_(std::move(traceId)),
      coalescingKey_(std::move(coalescingKey))
{
}

EventKind EventEnvelope::kind() const noexcept { return kind_; }
const kernel::EventName& EventEnvelope::name() const noexcept { return name_; }
const foundation::Version& EventEnvelope::version() const noexcept { return version_; }
const foundation::Value& EventEnvelope::payload() const noexcept { return payload_; }
const std::optional<kernel::ObjectId>& EventEnvelope::aggregateId() const noexcept
{
    return aggregateId_;
}
const std::optional<kernel::TransactionId>& EventEnvelope::transactionId() const noexcept
{
    return transactionId_;
}
const std::optional<kernel::ProjectId>& EventEnvelope::projectId() const noexcept
{
    return projectId_;
}
const std::optional<kernel::DocumentId>& EventEnvelope::documentId() const noexcept
{
    return documentId_;
}
const std::optional<state::RevisionSet>& EventEnvelope::revisions() const noexcept
{
    return revisions_;
}
const std::optional<std::size_t>& EventEnvelope::sequence() const noexcept { return sequence_; }
const std::optional<kernel::CorrelationId>& EventEnvelope::correlationId() const noexcept
{
    return correlationId_;
}
const std::optional<kernel::TraceId>& EventEnvelope::traceId() const noexcept { return traceId_; }
const std::optional<std::string>& EventEnvelope::coalescingKey() const noexcept
{
    return coalescingKey_;
}

EventSubscription::EventSubscription(
    std::weak_ptr<detail::EventBusCore> core,
    kernel::SubscriptionId subscriptionId)
    : core_(std::move(core)), subscriptionId_(std::move(subscriptionId))
{
}

EventSubscription::~EventSubscription() { cancel(); }

EventSubscription::EventSubscription(EventSubscription&& other) noexcept
    : core_(std::move(other.core_)), subscriptionId_(std::move(other.subscriptionId_))
{
    other.core_.reset();
}

EventSubscription& EventSubscription::operator=(EventSubscription&& other) noexcept
{
    if(this != &other) {
        cancel();
        core_ = std::move(other.core_);
        subscriptionId_ = std::move(other.subscriptionId_);
        other.core_.reset();
    }
    return *this;
}

const kernel::SubscriptionId& EventSubscription::id() const noexcept
{
    return subscriptionId_;
}

void EventSubscription::cancel() noexcept
{
    const auto core = core_.lock();
    if(core != nullptr) {
        std::lock_guard lock(core->mutex);
        core->subscriptions.erase(subscriptionId_);
    }
    core_.reset();
}

EventBus::EventBus() : core_(std::make_shared<detail::EventBusCore>()) {}

EventBus::~EventBus()
{
    std::lock_guard lock(core_->mutex);
    core_->closed = true;
    core_->subscriptions.clear();
    core_->queued.clear();
}

foundation::Result<EventSubscription> EventBus::subscribe(
    kernel::SubscriptionId subscriptionId,
    EventFilter filter,
    DeliveryMode mode,
    EventCallback callback)
{
    if(!callback) {
        return foundation::Result<EventSubscription>::failure(eventError(
            "Event.InvalidSubscriber",
            foundation::ErrorCategory::Validation,
            "An event callback is required",
            subscriptionId));
    }
    std::lock_guard lock(core_->mutex);
    if(core_->closed) {
        return foundation::Result<EventSubscription>::failure(eventError(
            "Event.BusClosed",
            foundation::ErrorCategory::Conflict,
            "The event bus is closed",
            subscriptionId));
    }
    const auto id = subscriptionId;
    const auto [unused, inserted] = core_->subscriptions.emplace(
        id,
        detail::EventBusCore::SubscriptionEntry {
            std::move(filter), mode, std::move(callback)});
    static_cast<void>(unused);
    if(!inserted) {
        return foundation::Result<EventSubscription>::failure(eventError(
            "Event.SubscriptionAlreadyExists",
            foundation::ErrorCategory::Conflict,
            "An event subscription with the same stable ID already exists",
            id));
    }
    return foundation::Result<EventSubscription>::success(
        EventSubscription {core_, std::move(subscriptionId)});
}

foundation::Result<EventDeliveryReport> EventBus::publish(
    const CommittedDomainEvent& event,
    kernel::CorrelationId correlationId,
    kernel::TraceId traceId)
{
    return publishEnvelope(EventEnvelope(
        EventKind::Domain,
        event.name(),
        event.version(),
        event.payload(),
        event.aggregateId(),
        event.transactionId(),
        event.projectId(),
        event.documentId(),
        event.revisions(),
        event.sequence(),
        std::move(correlationId),
        std::move(traceId),
        std::nullopt));
}

foundation::Result<EventDeliveryReport> EventBus::publish(
    const TransientEvent& event,
    std::optional<kernel::CorrelationId> correlationId,
    std::optional<kernel::TraceId> traceId)
{
    return publishEnvelope(EventEnvelope(
        event.kind(),
        event.name(),
        event.version(),
        event.payload(),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::move(correlationId),
        std::move(traceId),
        event.coalescingKey()));
}

foundation::Result<EventDeliveryReport> EventBus::publishEnvelope(EventEnvelope envelope)
{
    try {
        using ImmediateDelivery = std::pair<kernel::SubscriptionId, EventCallback>;
        std::vector<ImmediateDelivery> immediate;
        EventDeliveryReport report;
        {
            std::lock_guard lock(core_->mutex);
            if(core_->closed) {
                return foundation::Result<EventDeliveryReport>::failure(foundation::makeError(
                    "Event.BusClosed",
                    foundation::ErrorCategory::Conflict,
                    "The event bus is closed"));
            }
            auto nextQueue = core_->queued;
            for(const auto& [subscriptionId, subscription] : core_->subscriptions) {
                if(!matches(subscription.filter, envelope)) {
                    continue;
                }
                ++report.matched;
                if(subscription.mode == DeliveryMode::Immediate) {
                    immediate.emplace_back(subscriptionId, subscription.callback);
                    continue;
                }

                bool coalesced = false;
                if(envelope.kind() == EventKind::Notification
                   && envelope.coalescingKey().has_value()) {
                    const auto queued = std::find_if(
                        nextQueue.rbegin(),
                        nextQueue.rend(),
                        [&](const detail::EventBusCore::QueuedDelivery& pending) {
                            return pending.subscriptionId == subscriptionId
                                && pending.envelope.kind() == EventKind::Notification
                                && pending.envelope.name() == envelope.name()
                                && pending.envelope.coalescingKey() == envelope.coalescingKey();
                        });
                    if(queued != nextQueue.rend()) {
                        queued->envelope = envelope;
                        coalesced = true;
                        ++report.coalesced;
                    }
                }
                if(!coalesced) {
                    nextQueue.push_back(
                        detail::EventBusCore::QueuedDelivery {subscriptionId, envelope});
                    ++report.queued;
                }
            }
            core_->queued.swap(nextQueue);
        }

        report.failures.reserve(immediate.size());
        for(const auto& [subscriptionId, callback] : immediate) {
            try {
                callback(envelope);
                ++report.delivered;
            } catch(const std::exception& exception) {
                report.failures.push_back(callbackFailure(subscriptionId, exception.what()));
            } catch(...) {
                report.failures.push_back(callbackFailure(subscriptionId, "Unknown failure"));
            }
        }
        return foundation::Result<EventDeliveryReport>::success(std::move(report));
    } catch(const std::exception& exception) {
        return foundation::Result<EventDeliveryReport>::failure(foundation::makeError(
            "Event.PublishFailed",
            foundation::ErrorCategory::Internal,
            "The event could not be prepared for delivery",
            foundation::Value {foundation::Value::Object {
                {"reason", foundation::Value {exception.what()}},
            }}));
    } catch(...) {
        return foundation::Result<EventDeliveryReport>::failure(foundation::makeError(
            "Event.PublishFailed",
            foundation::ErrorCategory::Internal,
            "The event could not be prepared for delivery"));
    }
}

EventDeliveryReport EventBus::drainQueued(std::size_t maximumDeliveries)
{
    EventDeliveryReport report;
    std::vector<detail::EventBusCore::QueuedDelivery> deliveries;
    {
        std::lock_guard lock(core_->mutex);
        const auto count = std::min(maximumDeliveries, core_->queued.size());
        deliveries.reserve(count);
        for(std::size_t index = 0; index < count; ++index) {
            deliveries.push_back(std::move(core_->queued.front()));
            core_->queued.pop_front();
        }
    }

    report.matched = deliveries.size();
    for(const auto& delivery : deliveries) {
        EventCallback callback;
        {
            std::lock_guard lock(core_->mutex);
            const auto subscription = core_->subscriptions.find(delivery.subscriptionId);
            if(subscription == core_->subscriptions.end()) {
                continue;
            }
            callback = subscription->second.callback;
        }
        try {
            callback(delivery.envelope);
            ++report.delivered;
        } catch(const std::exception& exception) {
            report.failures.push_back(
                callbackFailure(delivery.subscriptionId, exception.what()));
        } catch(...) {
            report.failures.push_back(
                callbackFailure(delivery.subscriptionId, "Unknown failure"));
        }
    }
    return report;
}

std::size_t EventBus::subscriptionCount() const
{
    std::lock_guard lock(core_->mutex);
    return core_->subscriptions.size();
}

std::size_t EventBus::queuedCount() const
{
    std::lock_guard lock(core_->mutex);
    return core_->queued.size();
}

} // namespace lasercnc::messaging
