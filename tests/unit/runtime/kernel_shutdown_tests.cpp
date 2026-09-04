#include <lasercnc/kernel/app_kernel.hpp>
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <functional>
#include <future>
#include <stdexcept>

using namespace lasercnc;
using namespace std::chrono_literals;
namespace {
template<typename Id> Id id(const char* value) { return Id::create(value).value(); }
class ScriptedStopExecutor final : public platform::ITaskExecutor {
public:
    unsigned int calls{0U};
    unsigned int failures{0U};
    bool throws{false};
    std::function<void()> onStop;
    foundation::Result<void> submit(platform::ExecutorWork work, platform::ExecutorCompletion completion) override
    { completion(work()); return foundation::Result<void>::success(); }
    foundation::Result<void> waitIdle() override { return foundation::Result<void>::success(); }
    foundation::Result<void> shutdown() override
    {
        ++calls;
        if(onStop) { onStop(); }
        if(calls <= failures) {
            if(throws) { throw std::runtime_error("Injected executor stop exception"); }
            return foundation::Result<void>::failure(foundation::makeError(
                "Test.ExecutorStopFailed", foundation::ErrorCategory::Infrastructure, "Injected executor stop failure"));
        }
        return foundation::Result<void>::success();
    }
    std::size_t concurrency() const noexcept override { return 1U; }
};
class StopObserver final : public kernel::IModule {
public:
    unsigned int calls{0U};
    const kernel::ModuleDescriptor& descriptor() const noexcept override { return descriptor_; }
    foundation::Result<void> stop(kernel::AppKernel&) override { ++calls; return foundation::Result<void>::success(); }
private:
    kernel::ModuleDescriptor descriptor_{id<kernel::ModuleId>("module.stop-observer"), "Stop observer", {1U, 0U, 0U}};
};
template<typename Stop> foundation::Result<void> observedStop(Stop stop)
{
    try { return stop(); }
    catch(...) {
        CHECK(false);
        return foundation::Result<void>::failure(foundation::makeError(
            "Test.ExceptionEscaped", foundation::ErrorCategory::Internal, "Shutdown leaked an executor exception"));
    }
}
}

TEST_CASE("Shutdown acknowledgement Kernel retries failed executor stops before revoking modules", "[kernel-shutdown]")
{
    for(bool ready : {false, true}) {
        for(bool throws : {false, true}) {
            CAPTURE(ready, throws);
            kernel::AppKernel host;
            auto owned = std::make_unique<ScriptedStopExecutor>();
            auto& executor = *owned;
            executor.failures = 2U;
            executor.throws = throws;
            REQUIRE(host.configureTaskExecutor(std::move(owned)));
            auto module = std::make_unique<StopObserver>();
            auto& observer = *module;
            REQUIRE(host.addModule(std::move(module)));
            if(ready) { REQUIRE(host.bootstrap()); }
            for(unsigned int attempt = 1U; attempt <= 2U; ++attempt) {
                auto stopped = observedStop([&] { return host.shutdown(1s); });
                CHECK_FALSE(stopped);
                CHECK(executor.calls == attempt);
                CHECK(host.state() == kernel::AppKernelState::Stopping);
                CHECK(observer.calls == 0U);
            }
            REQUIRE(host.shutdown(1s));
            CHECK(executor.calls == 3U);
            CHECK(observer.calls == (ready ? 1U : 0U));
            CHECK(host.state() == kernel::AppKernelState::Stopped);
            REQUIRE(host.shutdown(1s));
            CHECK(executor.calls == 3U);
        }
    }
}

TEST_CASE("Shutdown acknowledgement standalone scheduler retries even before start", "[kernel-shutdown]")
{
    for(bool started : {false, true}) {
        for(bool throws : {false, true}) {
            CAPTURE(started, throws);
            runtime::ResourceManager resources;
            observability::LocalTraceService traces;
            observability::LocalMetricsService metrics;
            ScriptedStopExecutor executor;
            runtime::Scheduler scheduler{resources, traces, metrics};
            REQUIRE(scheduler.configureExecutor(executor));
            if(started) { REQUIRE(scheduler.start()); }
            executor.failures = 2U;
            executor.throws = throws;
            for(unsigned int attempt = 1U; attempt <= 2U; ++attempt) {
                CHECK_FALSE(observedStop([&] { return scheduler.shutdown(1s); }));
                CHECK(executor.calls == attempt);
            }
            REQUIRE(scheduler.shutdown(1s));
            CHECK(executor.calls == 3U);
            REQUIRE(scheduler.shutdown(1s));
            CHECK(executor.calls == 3U);
        }
    }
}

TEST_CASE("Shutdown acknowledgement stopped schedulers cannot restart their drained executor", "[kernel-shutdown]")
{
    runtime::ResourceManager resources;
    observability::LocalTraceService traces;
    observability::LocalMetricsService metrics;
    ScriptedStopExecutor executor;
    runtime::Scheduler scheduler{resources, traces, metrics};
    REQUIRE(scheduler.configureExecutor(executor));
    REQUIRE(scheduler.start());
    REQUIRE(scheduler.shutdown(1s));
    CHECK_FALSE(scheduler.start());
}

TEST_CASE("Shutdown acknowledgement invalid timeout never seals Kernel admission", "[kernel-shutdown]")
{
    for(bool configured : {false, true}) {
        kernel::AppKernel host;
        if(configured) { REQUIRE(host.configureTaskExecutor(std::make_unique<ScriptedStopExecutor>())); }
        REQUIRE(host.bootstrap());
        CHECK_FALSE(host.shutdown(-1ms));
        CHECK(host.state() == kernel::AppKernelState::Ready);
        CHECK(host.projectRuntime().create(id<kernel::ProjectId>("project.after-invalid-timeout")));
        REQUIRE(host.shutdown(1s));
    }
}

TEST_CASE("Shutdown acknowledgement rejects reentrant executor shutdown", "[kernel-shutdown]")
{
    runtime::ResourceManager resources;
    observability::LocalTraceService traces;
    observability::LocalMetricsService metrics;
    ScriptedStopExecutor executor;
    runtime::Scheduler scheduler{resources, traces, metrics};
    REQUIRE(scheduler.configureExecutor(executor));
    executor.onStop = [&] {
        auto nested = scheduler.shutdown(1s);
        CHECK_FALSE(nested);
        if(!nested) { CHECK(std::string(nested.error().code.value()) == "Task.ShutdownInProgress"); }
        CHECK_FALSE(scheduler.start());
    };
    REQUIRE(scheduler.shutdown(1s));
    CHECK(executor.calls == 1U);
}

TEST_CASE("Shutdown acknowledgement concurrent callers cannot bypass pending drain", "[kernel-shutdown]")
{
    runtime::ResourceManager resources;
    observability::LocalTraceService traces;
    observability::LocalMetricsService metrics;
    ScriptedStopExecutor executor;
    runtime::Scheduler scheduler{resources, traces, metrics};
    REQUIRE(scheduler.configureExecutor(executor));
    REQUIRE(scheduler.start());
    std::promise<void> entered;
    auto entry = entered.get_future();
    std::promise<void> released;
    auto release = released.get_future().share();
    std::atomic_bool gateExpired{false};
    executor.onStop = [&] {
        entered.set_value();
        gateExpired.store(release.wait_for(5s) != std::future_status::ready);
    };
    auto first = std::async(std::launch::async, [&] { return scheduler.shutdown(1s); });
    const auto seen = entry.wait_for(5s);
    auto second = scheduler.shutdown(1ms);
    released.set_value();
    auto firstResult = first.get();
    REQUIRE(seen == std::future_status::ready);
    CHECK_FALSE(second);
    if(!second) { CHECK(std::string(second.error().code.value()) == "Task.ShutdownInProgress"); }
    CHECK(firstResult);
    CHECK_FALSE(gateExpired.load());
    CHECK(executor.calls == 1U);
    CHECK(scheduler.shutdown(1s));
}

TEST_CASE("Shutdown acknowledgement empty scheduler seals configuration and handles maximum timeout", "[kernel-shutdown]")
{
    runtime::ResourceManager resources;
    observability::LocalTraceService traces;
    observability::LocalMetricsService metrics;
    ScriptedStopExecutor executor;
    runtime::Scheduler scheduler{resources, traces, metrics};
    CHECK_FALSE(scheduler.shutdown(-1ms));
    REQUIRE(scheduler.shutdown(std::chrono::milliseconds::max()));
    CHECK_FALSE(scheduler.configureExecutor(executor));
    CHECK_FALSE(scheduler.start());
    REQUIRE(scheduler.shutdown(0ms));
    CHECK(executor.calls == 0U);
}
