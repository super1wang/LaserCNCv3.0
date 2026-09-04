#include <lasercnc/messaging/event_bus.hpp>
#include <lasercnc/runtime/transaction_manager.hpp>
#include <lasercnc/state/document_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
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

// Bound a real deadlock regression in its CTest process without detaching unsafe workers.
// 中文翻译：在独立 CTest 进程内为真实死锁回归设上限，不分离持有悬空引用的工作线程。
class EventReentryDeadline final {
public:
    EventReentryDeadline() : worker_([ready = finished_.get_future()] {
        if(ready.wait_for(std::chrono::seconds{10}) != std::future_status::ready) {
            std::fputs("event-reentry-deadline-exceeded\n", stderr);
            std::fflush(stderr);
            std::_Exit(86);
        }
    }) {}
    ~EventReentryDeadline() { finished_.set_value(); }
private:
    std::promise<void> finished_;
    std::jthread worker_;
};

struct EventCapture final {
    std::function<void()> onDestroy;
    ~EventCapture() { if(onDestroy) { onDestroy(); } }
};

struct EventCopyState final {
    std::function<void()> onCopy;
    unsigned int calls{0U};
};

struct EventCopyObserver final {
    std::shared_ptr<EventCopyState> state;
    std::shared_ptr<EventCapture> capture;
    explicit EventCopyObserver(std::shared_ptr<EventCopyState> value, std::shared_ptr<EventCapture> owned = {})
        : state(std::move(value)), capture(std::move(owned)) {}
    EventCopyObserver(const EventCopyObserver& other) : state(other.state), capture(other.capture)
    {
        if(state->onCopy) { state->onCopy(); }
    }
    EventCopyObserver(EventCopyObserver&&) noexcept = default;
    void operator()(const EventEnvelope&) const { ++state->calls; }
};

void checkEventCopyReentry(DeliveryMode mode)
{
    EventReentryDeadline deadline;
    EventBus bus;
    auto state = std::make_shared<EventCopyState>();
    auto token = bus.subscribe(validId<SubscriptionId>("subscription.copy-reentry"), {}, mode,
        EventCopyObserver{state});
    REQUIRE(token);
    unsigned int copies = 0U;
    state->onCopy = [&] {
        ++copies;
        if(bus.subscriptionCount() != 1U) { throw std::runtime_error("lost subscription"); }
    };
    const auto published = bus.publish(TransientEvent::system(validId<EventName>("copy.event"), Version{1U, 0U, 0U}, {}));
    REQUIRE(published);
    const auto report = mode == DeliveryMode::Immediate ? published.value() : bus.drainQueued();
    CHECK(report.delivered == 1U);
    CHECK(report.failures.empty());
    CHECK(copies == 1U);
    CHECK(state->calls == 1U);
}

void checkEventCopyFailures(DeliveryMode mode)
{
    for(unsigned int failure = 0U; failure < 3U; ++failure) {
        DYNAMIC_SECTION("copy failure=" << failure) {
            EventBus bus;
            auto state = std::make_shared<EventCopyState>();
            auto failing = bus.subscribe(validId<SubscriptionId>("subscription.a-copy-failure"), {}, mode,
                EventCopyObserver{state});
            REQUIRE(failing);
            unsigned int healthyCalls = 0U;
            auto healthy = bus.subscribe(validId<SubscriptionId>("subscription.z-healthy"), {}, mode,
                [&](const EventEnvelope&) { ++healthyCalls; });
            REQUIRE(healthy);
            state->onCopy = [failure] {
                if(failure == 0U) { throw std::runtime_error("copy failed"); }
                if(failure == 1U) { throw 42; }
                throw std::bad_alloc{};
            };
            const auto event = TransientEvent::system(validId<EventName>("copy.event"), Version{1U, 0U, 0U}, {});
            auto published = bus.publish(event);
            REQUIRE(published);
            const auto report = mode == DeliveryMode::Immediate ? published.value() : bus.drainQueued();
            CHECK(report.matched == 2U);
            CHECK(report.delivered == 1U);
            REQUIRE(report.failures.size() == 1U);
            CHECK(report.failures.front().subscriptionId == failing.value().id());
            CHECK(std::string(report.failures.front().error.code.value()) == "Event.SubscriberFailed");
            CHECK(state->calls == 0U);
            CHECK(healthyCalls == 1U);
            CHECK(bus.queuedCount() == 0U);
            state->onCopy = {};
            published = bus.publish(event);
            REQUIRE(published);
            const auto retry = mode == DeliveryMode::Immediate ? published.value() : bus.drainQueued();
            CHECK(retry.delivered == 2U);
            CHECK(retry.failures.empty());
            CHECK(state->calls == 1U);
            CHECK(healthyCalls == 2U);
        }
    }
}

} // namespace

TEST_CASE("EventBus cancellation releases captures outside its lock", "[messaging][event][c6b5]")
{
    EventReentryDeadline deadline;
    EventBus bus;
    unsigned int destroyed = 0U;
    std::size_t observed = 99U;
    std::optional<EventSubscription> token;
    std::optional<EventSubscription> replacement;
    const auto id = validId<SubscriptionId>("subscription.cancel-reentry");
    auto capture = std::make_shared<EventCapture>();
    capture->onDestroy = [&] {
        ++destroyed;
        observed = bus.subscriptionCount();
        auto next = bus.subscribe(id, {}, DeliveryMode::Immediate, [](const EventEnvelope&) {});
        if(next) { replacement.emplace(std::move(next).value()); }
        token->cancel();
    };
    auto subscribed = bus.subscribe(id, {}, DeliveryMode::Immediate,
        [owned = std::move(capture)](const EventEnvelope&) { static_cast<void>(owned); });
    REQUIRE(subscribed);
    token.emplace(std::move(subscribed).value());
    token->cancel();
    CHECK(destroyed == 1U);
    CHECK(observed == 0U);
    REQUIRE(replacement);
    CHECK(bus.subscriptionCount() == 1U);
    token->cancel();
    CHECK(bus.subscriptionCount() == 1U);
}

TEST_CASE("EventBus duplicate rejection releases captures outside its lock", "[messaging][event][c6b5]")
{
    EventReentryDeadline deadline;
    EventBus bus;
    auto kept = bus.subscribe(validId<SubscriptionId>("subscription.duplicate"), {}, DeliveryMode::Immediate,
        [](const EventEnvelope&) {});
    REQUIRE(kept);
    unsigned int destroyed = 0U;
    std::size_t observed = 99U;
    auto capture = std::make_shared<EventCapture>();
    capture->onDestroy = [&] { ++destroyed; observed = bus.subscriptionCount(); };
    const auto rejected = bus.subscribe(kept.value().id(), {}, DeliveryMode::Immediate,
        [owned = std::move(capture)](const EventEnvelope&) { static_cast<void>(owned); });
    REQUIRE_FALSE(rejected);
    CHECK(std::string(rejected.error().code.value()) == "Event.SubscriptionAlreadyExists");
    CHECK(destroyed == 1U);
    CHECK(observed == 1U);
}

TEST_CASE("EventBus destruction releases captures after closing the registry", "[messaging][event][c6b5]")
{
    EventReentryDeadline deadline;
    unsigned int destroyed = 0U;
    std::optional<EventSubscription> token;
    auto bus = std::make_unique<EventBus>();
    auto capture = std::make_shared<EventCapture>();
    capture->onDestroy = [&] { ++destroyed; token->cancel(); };
    auto subscribed = bus->subscribe(validId<SubscriptionId>("subscription.destruct-reentry"), {}, DeliveryMode::Queued,
        [owned = std::move(capture)](const EventEnvelope&) { static_cast<void>(owned); });
    REQUIRE(subscribed);
    token.emplace(std::move(subscribed).value());
    REQUIRE(bus->publish(TransientEvent::system(validId<EventName>("destroy.event"), Version{1U, 0U, 0U}, {})));
    bus.reset();
    CHECK(destroyed == 1U);
    token->cancel();
}

TEST_CASE("EventBus immediate callback copies may reenter", "[messaging][event][c6b5]")
{
    checkEventCopyReentry(DeliveryMode::Immediate);
}

TEST_CASE("EventBus queued callback copies may reenter", "[messaging][event][c6b5]")
{
    checkEventCopyReentry(DeliveryMode::Queued);
}

TEST_CASE("EventBus isolates immediate callback copy failures", "[messaging][event][c6b5]")
{
    checkEventCopyFailures(DeliveryMode::Immediate);
}

TEST_CASE("EventBus isolates queued callback copy failures", "[messaging][event][c6b5]")
{
    checkEventCopyFailures(DeliveryMode::Queued);
}

TEST_CASE("EventBus preserves independent mutable callback copies per delivery", "[messaging][event][c6b5]")
{
    for(const auto mode : {DeliveryMode::Immediate, DeliveryMode::Queued}) {
        DYNAMIC_SECTION("mode=" << static_cast<unsigned int>(mode)) {
            EventBus bus;
            std::vector<unsigned int> observed;
            auto token = bus.subscribe(validId<SubscriptionId>("subscription.mutable-copy"), {}, mode,
                [local = 0U, &observed](const EventEnvelope&) mutable { observed.push_back(++local); });
            REQUIRE(token);
            const auto event = TransientEvent::system(validId<EventName>("mutable.event"), Version{1U, 0U, 0U}, {});
            REQUIRE(bus.publish(event));
            REQUIRE(bus.publish(event));
            static_cast<void>(bus.drainQueued());
            CHECK(observed == std::vector<unsigned int>{1U, 1U});
        }
    }
}

TEST_CASE("EventBus callback copy cancellation retains its captured incarnation", "[messaging][event][c6b5]")
{
    for(const auto mode : {DeliveryMode::Immediate, DeliveryMode::Queued}) {
        DYNAMIC_SECTION("mode=" << static_cast<unsigned int>(mode)) {
            EventReentryDeadline deadline;
            EventBus bus;
            auto state = std::make_shared<EventCopyState>();
            const auto id = validId<SubscriptionId>("subscription.copy-cancel");
            auto token = bus.subscribe(id, {}, mode, EventCopyObserver{state});
            REQUIRE(token);
            std::optional<EventSubscription> replacement;
            unsigned int newCalls = 0U;
            state->onCopy = [&] {
                token.value().cancel();
                auto next = bus.subscribe(id, {}, mode, [&](const EventEnvelope&) { ++newCalls; });
                if(!next) { throw std::runtime_error("replacement failed"); }
                replacement.emplace(std::move(next).value());
            };
            const auto event = TransientEvent::system(validId<EventName>("copy-cancel.event"), Version{1U, 0U, 0U}, {});
            auto published = bus.publish(event);
            REQUIRE(published);
            const auto report = mode == DeliveryMode::Immediate ? published.value() : bus.drainQueued();
            CHECK(report.delivered == 1U);
            CHECK(report.failures.empty());
            CHECK(state->calls == 1U);
            CHECK(newCalls == 0U);
            REQUIRE(replacement);
            published = bus.publish(event);
            REQUIRE(published);
            static_cast<void>(bus.drainQueued());
            CHECK(newCalls == 1U);
            CHECK(state->calls == 1U);
        }
    }
}

TEST_CASE("EventBus copy failure preserves mixed delivery and later queued candidates", "[messaging][event][c6b5]")
{
    EventBus bus;
    auto state = std::make_shared<EventCopyState>();
    auto faulty = bus.subscribe(validId<SubscriptionId>("subscription.a-faulty"), {}, DeliveryMode::Immediate,
        EventCopyObserver{state});
    REQUIRE(faulty);
    unsigned int immediateCalls = 0U;
    auto immediate = bus.subscribe(validId<SubscriptionId>("subscription.b-immediate"), {}, DeliveryMode::Immediate,
        [&](const EventEnvelope&) { ++immediateCalls; });
    REQUIRE(immediate);
    std::vector<Value> received;
    auto queued = bus.subscribe(validId<SubscriptionId>("subscription.z-queued"), {}, DeliveryMode::Queued,
        [&](const EventEnvelope& event) { received.push_back(event.payload()); });
    REQUIRE(queued);
    state->onCopy = [] { throw std::runtime_error("mixed copy failure"); };
    for(const char* payload : {"first", "second"}) {
        const auto report = bus.publish(TransientEvent::system(validId<EventName>("mixed.event"), Version{1U, 0U, 0U}, Value{payload}));
        REQUIRE(report);
        CHECK(report.value().matched == 3U);
        CHECK(report.value().delivered == 1U);
        CHECK(report.value().queued == 1U);
        REQUIRE(report.value().failures.size() == 1U);
        CHECK(report.value().failures.front().subscriptionId == faulty.value().id());
    }
    CHECK(immediateCalls == 2U);
    CHECK(bus.queuedCount() == 2U);
    const auto drained = bus.drainQueued();
    CHECK(drained.matched == 2U);
    CHECK(drained.delivered == 2U);
    CHECK(drained.failures.empty());
    CHECK(received == std::vector<Value>{Value{"first"}, Value{"second"}});
}

TEST_CASE("EventBus retains callback resources while another thread cancels during copying", "[messaging][event][c6b5]")
{
    for(const auto mode : {DeliveryMode::Immediate, DeliveryMode::Queued}) {
        DYNAMIC_SECTION("mode=" << static_cast<unsigned int>(mode)) {
            EventReentryDeadline deadline;
            EventBus bus;
            auto state = std::make_shared<EventCopyState>();
            auto capture = std::make_shared<EventCapture>();
            std::weak_ptr<EventCapture> lifetime = capture;
            unsigned int destroyed = 0U;
            capture->onDestroy = [&] { ++destroyed; };
            auto token = bus.subscribe(validId<SubscriptionId>("subscription.copy-barrier"), {}, mode,
                EventCopyObserver{state, std::move(capture)});
            REQUIRE(token);
            std::promise<void> entered;
            auto enteredFuture = entered.get_future();
            std::promise<void> release;
            auto releaseFuture = release.get_future();
            state->onCopy = [&] {
                entered.set_value();
                if(releaseFuture.wait_for(std::chrono::seconds{5}) != std::future_status::ready) {
                    throw std::runtime_error("copy barrier timed out");
                }
            };
            const auto event = TransientEvent::system(validId<EventName>("copy-barrier.event"), Version{1U, 0U, 0U}, {});
            if(mode == DeliveryMode::Queued) { REQUIRE(bus.publish(event)); }
            EventDeliveryReport report;
            bool published = true;
            std::jthread worker([&] {
                if(mode == DeliveryMode::Queued) { report = bus.drainQueued(); }
                else {
                    auto result = bus.publish(event);
                    published = result.hasValue();
                    if(result) { report = std::move(result).value(); }
                }
            });
            const bool ready = enteredFuture.wait_for(std::chrono::seconds{5}) == std::future_status::ready;
            if(ready) { token.value().cancel(); }
            const bool retained = !lifetime.expired();
            release.set_value();
            worker.join();
            REQUIRE(ready);
            REQUIRE(published);
            CHECK(retained);
            CHECK(report.delivered == 1U);
            CHECK(report.failures.empty());
            CHECK(state->calls == 1U);
            CHECK(lifetime.expired());
            CHECK(destroyed == 1U);
            CHECK(bus.subscriptionCount() == 0U);
        }
    }
}

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

TEST_CASE("EventBus rejects every undefined delivery mode and filter kind", "[messaging][event][c6]")
{
    EventBus bus;
    auto kept = bus.subscribe(validId<SubscriptionId>("subscription.kept"), {}, DeliveryMode::Queued,
        [](const EventEnvelope&) {});
    REQUIRE(kept);
    REQUIRE(bus.publish(TransientEvent::system(validId<EventName>("event.kept"), Version{1U, 0U, 0U}, {})));
    for(unsigned int raw = 2U; raw <= 255U; ++raw) {
        INFO("mode=" << raw);
        auto result = bus.subscribe(validId<SubscriptionId>("subscription.invalid"), {},
            static_cast<DeliveryMode>(raw), [](const EventEnvelope&) {});
        CHECK_FALSE(result);
        if(!result) {
            CHECK(std::string(result.error().code.value()) == "Event.InvalidDeliveryMode");
            CHECK(result.error().category == ErrorCategory::Validation);
        }
        CHECK(bus.subscriptionCount() == 1U);
        CHECK(bus.queuedCount() == 1U);
    }
    for(unsigned int raw = 3U; raw <= 255U; ++raw) {
        for(const auto mode : {DeliveryMode::Immediate, DeliveryMode::Queued}) {
            INFO("filterKind=" << raw << " mode=" << static_cast<unsigned int>(mode));
            auto result = bus.subscribe(validId<SubscriptionId>("subscription.invalid"),
                EventFilter{static_cast<EventKind>(raw), std::nullopt}, mode, [](const EventEnvelope&) {});
            CHECK_FALSE(result);
            if(!result) {
                CHECK(std::string(result.error().code.value()) == "Event.InvalidFilterKind");
                CHECK(result.error().category == ErrorCategory::Validation);
            }
            CHECK(bus.subscriptionCount() == 1U);
            CHECK(bus.queuedCount() == 1U);
        }
    }
    CHECK(bus.drainQueued().delivered == 1U);
}

TEST_CASE("EventBus coalesces notifications only within an exact version", "[messaging][event][c6]")
{
    for(const auto changed : {Version{2U, 0U, 0U}, Version{1U, 1U, 0U}, Version{1U, 0U, 1U}}) {
        DYNAMIC_SECTION("version=" << changed.toString()) {
            EventBus bus;
            std::vector<EventEnvelope> received;
            auto token = bus.subscribe(validId<SubscriptionId>("subscription.version"), {}, DeliveryMode::Queued,
                [&](const EventEnvelope& event) { received.push_back(event); });
            REQUIRE(token);
            const auto name = validId<EventName>("task.progress");
            REQUIRE(bus.publish(TransientEvent::notification(name, Version{1U, 0U, 0U}, Value{"old"}, "same")));
            auto other = bus.publish(TransientEvent::notification(name, changed, Value{"other"}, "same"));
            REQUIRE(other);
            CHECK(other.value().queued == 1U);
            CHECK(other.value().coalesced == 0U);
            auto updated = bus.publish(TransientEvent::notification(name, Version{1U, 0U, 0U}, Value{"updated"}, "same"));
            REQUIRE(updated);
            CHECK(updated.value().coalesced == 1U);
            CHECK(bus.drainQueued().delivered == 2U);
            REQUIRE(received.size() == 2U);
            CHECK(received[0].version() == Version{1U, 0U, 0U});
            CHECK(received[0].payload() == Value{"updated"});
            CHECK(received[1].version() == changed);
            CHECK(received[1].payload() == Value{"other"});
        }
    }
}

TEST_CASE("EventBus never reassigns cancelled queued work to a reused subscription id", "[messaging][event][c6]")
{
    for(const auto newMode : {DeliveryMode::Immediate, DeliveryMode::Queued}) {
        DYNAMIC_SECTION("newMode=" << static_cast<unsigned int>(newMode)) {
            EventBus bus;
            unsigned int oldCalls = 0U;
            unsigned int newCalls = 0U;
            const auto id = validId<SubscriptionId>("subscription.reused");
            auto old = bus.subscribe(id, {}, DeliveryMode::Queued, [&](const EventEnvelope&) { ++oldCalls; });
            REQUIRE(old);
            REQUIRE(bus.publish(TransientEvent::system(validId<EventName>("old.event"), Version{1U, 0U, 0U}, {})));
            old.value().cancel();
            auto replacement = bus.subscribe(id, EventFilter{EventKind::System, validId<EventName>("new.event")},
                newMode, [&](const EventEnvelope&) { ++newCalls; });
            REQUIRE(replacement);
            CHECK(bus.drainQueued().delivered == 0U);
            CHECK(oldCalls == 0U);
            CHECK(newCalls == 0U);
            REQUIRE(bus.publish(TransientEvent::system(validId<EventName>("new.event"), Version{1U, 0U, 0U}, {})));
            static_cast<void>(bus.drainQueued());
            CHECK(newCalls == 1U);
        }
    }
}

TEST_CASE("EventBus retains subscription identity inside an extracted drain batch", "[messaging][event][c6]")
{
    EventBus bus;
    unsigned int oldCalls = 0U;
    unsigned int newCalls = 0U;
    std::optional<EventSubscription> old;
    std::optional<EventSubscription> replacement;
    const auto reused = validId<SubscriptionId>("subscription.z-reused");
    auto first = bus.subscribe(validId<SubscriptionId>("subscription.a-first"), {}, DeliveryMode::Queued,
        [&](const EventEnvelope&) {
            old->cancel();
            auto next = bus.subscribe(reused, {}, DeliveryMode::Queued, [&](const EventEnvelope&) { ++newCalls; });
            REQUIRE(next);
            replacement.emplace(std::move(next).value());
        });
    REQUIRE(first);
    auto second = bus.subscribe(reused, {}, DeliveryMode::Queued, [&](const EventEnvelope&) { ++oldCalls; });
    REQUIRE(second);
    old.emplace(std::move(second).value());
    REQUIRE(bus.publish(TransientEvent::system(validId<EventName>("batch.event"), Version{1U, 0U, 0U}, {})));
    const auto drained = bus.drainQueued();
    CHECK(drained.matched == 2U);
    CHECK(drained.delivered == 1U);
    CHECK(drained.failures.empty());
    CHECK(oldCalls == 0U);
    CHECK(newCalls == 0U);
}

TEST_CASE("EventBus does not coalesce notification work across subscription incarnations", "[messaging][event][c6]")
{
    EventBus bus;
    std::vector<Value> received;
    const auto id = validId<SubscriptionId>("subscription.incarnation");
    const auto name = validId<EventName>("task.progress");
    auto old = bus.subscribe(id, {}, DeliveryMode::Queued, [](const EventEnvelope&) {});
    REQUIRE(old);
    REQUIRE(bus.publish(TransientEvent::notification(name, Version{1U, 0U, 0U}, Value{"old"}, "same")));
    old.value().cancel();
    auto next = bus.subscribe(id, {}, DeliveryMode::Queued,
        [&](const EventEnvelope& event) { received.push_back(event.payload()); });
    REQUIRE(next);
    const auto published = bus.publish(TransientEvent::notification(name, Version{1U, 0U, 0U}, Value{"new"}, "same"));
    REQUIRE(published);
    CHECK(published.value().queued == 1U);
    CHECK(published.value().coalesced == 0U);
    CHECK(bus.drainQueued().delivered == 1U);
    REQUIRE(received.size() == 1U);
    CHECK(received.front() == Value{"new"});
}

TEST_CASE("EventBus preserves every declared delivery and filter combination", "[messaging][event][c6]")
{
    for(const auto mode : {DeliveryMode::Immediate, DeliveryMode::Queued}) {
        for(unsigned int filterIndex = 0U; filterIndex < 4U; ++filterIndex) {
            DYNAMIC_SECTION("mode=" << static_cast<unsigned int>(mode) << " filter=" << filterIndex) {
                EventBus bus;
                std::vector<EventKind> received;
                const auto kind = filterIndex == 3U ? std::optional<EventKind>{}
                    : std::optional<EventKind>{static_cast<EventKind>(filterIndex)};
                auto token = bus.subscribe(validId<SubscriptionId>("subscription.valid-matrix"),
                    EventFilter{kind, std::nullopt}, mode, [&](const EventEnvelope& event) { received.push_back(event.kind()); });
                REQUIRE(token);
                REQUIRE(bus.publish(committedEvent(), validId<CorrelationId>("correlation.matrix"), validId<TraceId>("trace.matrix")));
                REQUIRE(bus.publish(TransientEvent::notification(validId<EventName>("notification.matrix"), Version{1U, 0U, 0U}, {})));
                REQUIRE(bus.publish(TransientEvent::system(validId<EventName>("system.matrix"), Version{1U, 0U, 0U}, {})));
                const auto expected = filterIndex == 3U ? 3U : 1U;
                CHECK(received.size() == (mode == DeliveryMode::Immediate ? expected : 0U));
                CHECK(bus.drainQueued().delivered == (mode == DeliveryMode::Queued ? expected : 0U));
                CHECK(received.size() == expected);
                for(const auto observed : received) { CHECK((!kind || observed == *kind)); }
            }
        }
    }
}

TEST_CASE("EventBus isolates reused subscriptions while another thread drains", "[messaging][event][c6]")
{
    EventBus bus;
    unsigned int oldCalls = 0U;
    unsigned int newCalls = 0U;
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::promise<void> release;
    auto releaseFuture = release.get_future();
    auto first = bus.subscribe(validId<SubscriptionId>("subscription.a-barrier"), {}, DeliveryMode::Queued,
        [&](const EventEnvelope&) {
            entered.set_value();
            if(releaseFuture.wait_for(std::chrono::seconds{5}) != std::future_status::ready) {
                throw std::runtime_error("test drain barrier timed out");
            }
        });
    REQUIRE(first);
    const auto reused = validId<SubscriptionId>("subscription.z-concurrent");
    auto old = bus.subscribe(reused, {}, DeliveryMode::Queued, [&](const EventEnvelope&) { ++oldCalls; });
    REQUIRE(old);
    REQUIRE(bus.publish(TransientEvent::system(validId<EventName>("concurrent.event"), Version{1U, 0U, 0U}, {})));
    EventDeliveryReport report;
    std::jthread worker([&] { report = bus.drainQueued(); });
    const bool ready = enteredFuture.wait_for(std::chrono::seconds{5}) == std::future_status::ready;
    std::optional<EventSubscription> replacement;
    if(ready) {
        old.value().cancel();
        auto next = bus.subscribe(reused, {}, DeliveryMode::Queued, [&](const EventEnvelope&) { ++newCalls; });
        if(next) { replacement.emplace(std::move(next).value()); }
    }
    release.set_value();
    worker.join();
    REQUIRE(ready);
    REQUIRE(replacement);
    CHECK(report.matched == 2U);
    CHECK(report.delivered == 1U);
    CHECK(report.failures.empty());
    CHECK(oldCalls == 0U);
    CHECK(newCalls == 0U);
}

TEST_CASE("EventBus cancellation does not transfer already captured immediate callbacks", "[messaging][event][c6]")
{
    EventBus bus;
    unsigned int oldCalls = 0U;
    unsigned int newCalls = 0U;
    std::optional<EventSubscription> old;
    std::optional<EventSubscription> replacement;
    const auto reused = validId<SubscriptionId>("subscription.z-immediate");
    auto first = bus.subscribe(validId<SubscriptionId>("subscription.a-immediate"), {}, DeliveryMode::Immediate,
        [&](const EventEnvelope&) {
            old->cancel();
            auto next = bus.subscribe(reused, {}, DeliveryMode::Immediate, [&](const EventEnvelope&) { ++newCalls; });
            REQUIRE(next);
            replacement.emplace(std::move(next).value());
        });
    REQUIRE(first);
    auto second = bus.subscribe(reused, {}, DeliveryMode::Immediate, [&](const EventEnvelope&) { ++oldCalls; });
    REQUIRE(second);
    old.emplace(std::move(second).value());
    const auto report = bus.publish(TransientEvent::system(validId<EventName>("immediate.event"), Version{1U, 0U, 0U}, {}));
    REQUIRE(report);
    CHECK(report.value().delivered == 2U);
    CHECK(report.value().failures.empty());
    CHECK(oldCalls == 1U);
    CHECK(newCalls == 0U);
}
