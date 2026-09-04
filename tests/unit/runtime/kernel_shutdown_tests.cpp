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
    bool drained{false};
    std::function<void()> onStop;
    std::function<void()> onDrain;
    foundation::Result<void> submit(platform::ExecutorWork work, platform::ExecutorCompletion completion) override
    {
        if(drained) { return foundation::Result<void>::failure(foundation::makeError(
            "Test.ExecutorStopped", foundation::ErrorCategory::Conflict, "stopped")); }
        completion(work()); return foundation::Result<void>::success();
    }
    foundation::Result<void> waitIdle() override { return foundation::Result<void>::success(); }
    void drainForDestruction() noexcept override { drained = true; if(onDrain) { onDrain(); } }
    bool isCurrentWorkerThread() const noexcept override { return false; }
    foundation::Result<void> shutdown() override
    {
        ++calls;
        if(onStop) { onStop(); }
        if(calls <= failures) {
            if(throws) { throw std::runtime_error("Injected executor stop exception"); }
            return foundation::Result<void>::failure(foundation::makeError(
                "Test.ExecutorStopFailed", foundation::ErrorCategory::Infrastructure, "Injected executor stop failure"));
        }
        drained = true;
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

TEST_CASE("Kernel final drain runs before executor destruction despite persistent stop failure", "[kernel-final-drain]")
{
    class FailingExecutor final : public platform::ITaskExecutor {
    public:
        explicit FailingExecutor(std::vector<std::string>& order, bool throws) : order_(order), throws_(throws) {}
        ~FailingExecutor() override { order_.emplace_back("executor-destroyed"); }
        foundation::Result<void> submit(platform::ExecutorWork work, platform::ExecutorCompletion completion) override
        { completion(work()); return foundation::Result<void>::success(); }
        foundation::Result<void> waitIdle() override { return shutdown(); }
        foundation::Result<void> shutdown() override
        {
            if(throws_) { throw std::runtime_error("persistent stop exception"); }
            return foundation::Result<void>::failure(foundation::makeError(
                "Test.PersistentStopFailure", foundation::ErrorCategory::Infrastructure, "persistent stop failure"));
        }
        void drainForDestruction() noexcept override { order_.emplace_back("drained"); }
        bool isCurrentWorkerThread() const noexcept override { return false; }
        std::size_t concurrency() const noexcept override { return 1U; }
    private:
        std::vector<std::string>& order_;
        bool throws_;
    };
    for(bool ready : {false, true}) {
        for(bool throws : {false, true}) {
            std::vector<std::string> order;
            {
                kernel::AppKernel host;
                REQUIRE(host.configureTaskExecutor(std::make_unique<FailingExecutor>(order, throws)));
                if(ready) { REQUIRE(host.bootstrap()); }
            }
            CHECK(order == std::vector<std::string>{"drained", "executor-destroyed"});
        }
    }
}

TEST_CASE("Kernel final drain covers failed bootstrap failed module stop and configuring states", "[kernel-final-drain]")
{
    class Module final : public kernel::IModule {
    public:
        bool failStart{false}, failStop{false};
        std::function<void()> stopping;
        const kernel::ModuleDescriptor& descriptor() const noexcept override { return descriptor_; }
        foundation::Result<void> start(kernel::AppKernel&) override { return outcome(failStart); }
        foundation::Result<void> stop(kernel::AppKernel&) override { stopping(); return outcome(failStop); }
    private:
        static foundation::Result<void> outcome(bool fail)
        {
            if(fail) { return foundation::Result<void>::failure(foundation::makeError(
                "Test.ModuleLifecycleFailure", foundation::ErrorCategory::Infrastructure, "expected module failure")); }
            return foundation::Result<void>::success();
        }
        kernel::ModuleDescriptor descriptor_{id<kernel::ModuleId>("module.final-drain"), "Final drain", {1U, 0U, 0U}};
    };
    for(int scenario = 0; scenario != 5; ++scenario) {
        CAPTURE(scenario);
        unsigned int drains = 0U, stops = 0U;
        bool drainBeforeStop = false;
        {
            kernel::AppKernel host;
            auto executor = std::make_unique<ScriptedStopExecutor>();
            executor->onDrain = [&] { ++drains; };
            if(scenario == 4) { executor->failures = 100U; }
            REQUIRE(host.configureTaskExecutor(std::move(executor)));
            auto module = std::make_unique<Module>();
            module->failStart = scenario == 2;
            module->failStop = scenario == 3;
            module->stopping = [&] { ++stops; drainBeforeStop = drains == 1U; };
            REQUIRE(host.addModule(std::move(module)));
            if(scenario != 0) {
                auto started = host.bootstrap();
                CHECK(started.hasValue() == (scenario != 2));
            }
            if(scenario == 3 || scenario == 4) { CHECK_FALSE(host.shutdown(0ms)); }
            if(scenario == 2 || scenario == 3) { CHECK(host.state() == kernel::AppKernelState::Failed); }
        }
        CHECK(drains == 1U);
        if(scenario == 0) { CHECK(stops == 0U); }
        if(scenario == 1 || scenario == 4) { CHECK(stops == 1U); CHECK(drainBeforeStop); }
        if(scenario == 3) { CHECK(stops == 1U); }
    }
}
