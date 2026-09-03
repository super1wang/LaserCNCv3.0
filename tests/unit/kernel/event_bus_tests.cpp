#include <lasercnc/messaging/event_bus.hpp>
#include <lasercnc/runtime/transaction_manager.hpp>
#include <lasercnc/state/document_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lasercnc::foundation;
using namespace lasercnc::kernel;
using namespace lasercnc::messaging;
using namespace lasercnc::runtime;
using namespace lasercnc::state;

namespace {

template <typename Id>
Id validId(const char* value)
{
    auto created = Id::create(value);
    if(!created.hasValue()) {
        throw std::logic_error("Invalid test ID");
    }
    return std::move(created).value();
}

CommittedDomainEvent committedEvent()
{
    DocumentStore store;
    const auto project = validId<ProjectId>("project.events");
    const auto document = validId<DocumentId>("document.events");
    REQUIRE(store.addDocument(project, document).hasValue());
    TransactionManager transactions(store);
    auto begun = transactions.begin(validId<TransactionId>("transaction.events"), document);
    REQUIRE(begun.hasValue());
    const auto object = validId<ObjectId>("object.events");
    REQUIRE(begun.value()->createObject(ObjectRecord {
        object, validId<ObjectTypeId>("kernel.test"), Value {"created"}}).hasValue());
    REQUIRE(begun.value()->collectEvent(PendingDomainEvent {
        validId<EventName>("document.object-created"),
        Version {1U, 0U, 0U},
        object,
        Value {"created"}}).hasValue());
    auto committed = begun.value()->commit();
    REQUIRE(committed.hasValue());
    REQUIRE(committed.value().events.size() == 1U);
    return committed.value().events.front();
}

} // namespace

TEST_CASE("EventBus delivers committed domain facts with trace context", "[messaging][event]")
{
    EventBus bus;
    std::vector<EventEnvelope> received;
    auto subscription = bus.subscribe(
        validId<SubscriptionId>("subscription.domain"),
        EventFilter {EventKind::Domain, validId<EventName>("document.object-created")},
        DeliveryMode::Immediate,
        [&](const EventEnvelope& event) { received.push_back(event); });
    REQUIRE(subscription.hasValue());

    auto report = bus.publish(
        committedEvent(),
        validId<CorrelationId>("correlation.events"),
        validId<TraceId>("trace.events"));
    REQUIRE(report.hasValue());
    CHECK(report.value().matched == 1U);
    CHECK(report.value().delivered == 1U);
    REQUIRE(received.size() == 1U);
    CHECK(received.front().kind() == EventKind::Domain);
    REQUIRE(received.front().transactionId().has_value());
    CHECK(*received.front().transactionId() == validId<TransactionId>("transaction.events"));
    REQUIRE(received.front().revisions().has_value());
    CHECK(received.front().revisions()->at(RevisionScope::Document) == Revision {1U});
    REQUIRE(received.front().traceId().has_value());
    CHECK(*received.front().traceId() == validId<TraceId>("trace.events"));
}

TEST_CASE("EventBus queues and coalesces notifications only", "[messaging][event]")
{
    EventBus bus;
    std::vector<std::string> values;
    auto subscription = bus.subscribe(
        validId<SubscriptionId>("subscription.notifications"),
        EventFilter {EventKind::Notification, std::nullopt},
        DeliveryMode::Queued,
        [&](const EventEnvelope& event) {
            values.push_back(*event.payload().getIf<std::string>());
        });
    REQUIRE(subscription.hasValue());

    auto first = bus.publish(TransientEvent::notification(
        validId<EventName>("task.progress"), Version {1U, 0U, 0U}, Value {"10"}, "task-1"));
    auto second = bus.publish(TransientEvent::notification(
        validId<EventName>("task.progress"), Version {1U, 0U, 0U}, Value {"20"}, "task-1"));
    auto third = bus.publish(TransientEvent::notification(
        validId<EventName>("task.progress"), Version {1U, 0U, 0U}, Value {"30"}, "task-2"));
    REQUIRE(first.hasValue());
    REQUIRE(second.hasValue());
    REQUIRE(third.hasValue());
    CHECK(first.value().queued == 1U);
    CHECK(second.value().coalesced == 1U);
    CHECK(bus.queuedCount() == 2U);

    const auto one = bus.drainQueued(1U);
    CHECK(one.delivered == 1U);
    REQUIRE(values.size() == 1U);
    CHECK(values.front() == "20");
    CHECK(bus.queuedCount() == 1U);
    CHECK(bus.drainQueued().delivered == 1U);
    REQUIRE(values.size() == 2U);
    CHECK(values.back() == "30");
}

TEST_CASE("EventBus contains subscriber failures and never holds its lock in callbacks", "[messaging][event]")
{
    EventBus bus;
    std::size_t nestedDeliveries = 0U;
    auto nested = bus.subscribe(
        validId<SubscriptionId>("subscription.nested"),
        EventFilter {EventKind::System, validId<EventName>("kernel.nested")},
        DeliveryMode::Immediate,
        [&](const EventEnvelope&) { ++nestedDeliveries; });
    REQUIRE(nested.hasValue());

    auto throwing = bus.subscribe(
        validId<SubscriptionId>("subscription.throwing"),
        EventFilter {EventKind::System, validId<EventName>("kernel.outer")},
        DeliveryMode::Immediate,
        [](const EventEnvelope&) { throw std::runtime_error("subscriber failed"); });
    REQUIRE(throwing.hasValue());
    auto reentrant = bus.subscribe(
        validId<SubscriptionId>("subscription.reentrant"),
        EventFilter {EventKind::System, validId<EventName>("kernel.outer")},
        DeliveryMode::Immediate,
        [&](const EventEnvelope&) {
            auto nestedResult = bus.publish(TransientEvent::system(
                validId<EventName>("kernel.nested"), Version {1U, 0U, 0U}, Value {}));
            if(!nestedResult.hasValue()) {
                throw std::runtime_error("nested publish failed");
            }
        });
    REQUIRE(reentrant.hasValue());

    auto report = bus.publish(TransientEvent::system(
        validId<EventName>("kernel.outer"), Version {1U, 0U, 0U}, Value {}));
    REQUIRE(report.hasValue());
    CHECK(report.value().matched == 2U);
    CHECK(report.value().delivered == 1U);
    REQUIRE(report.value().failures.size() == 1U);
    CHECK(std::string(report.value().failures.front().error.code.value())
          == "Event.SubscriberFailed");
    CHECK(nestedDeliveries == 1U);
}

TEST_CASE("Event subscription lifetime cancels pending delivery safely", "[messaging][event]")
{
    auto bus = std::make_unique<EventBus>();
    std::size_t delivered = 0U;
    auto subscription = bus->subscribe(
        validId<SubscriptionId>("subscription.lifetime"),
        EventFilter {},
        DeliveryMode::Queued,
        [&](const EventEnvelope&) { ++delivered; });
    REQUIRE(subscription.hasValue());
    REQUIRE(bus->publish(TransientEvent::system(
        validId<EventName>("kernel.shutdown"), Version {1U, 0U, 0U}, Value {})).hasValue());
    CHECK(bus->queuedCount() == 1U);
    subscription.value().cancel();
    CHECK(bus->subscriptionCount() == 0U);
    CHECK(bus->drainQueued().delivered == 0U);
    CHECK(delivered == 0U);

    auto survivingToken = bus->subscribe(
        validId<SubscriptionId>("subscription.outlives-bus"),
        EventFilter {},
        DeliveryMode::Immediate,
        [](const EventEnvelope&) {});
    REQUIRE(survivingToken.hasValue());
    bus.reset();
    survivingToken.value().cancel();
}
