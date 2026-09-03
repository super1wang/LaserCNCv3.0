#include <lasercnc/infrastructure/bs_thread_pool_executor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lasercnc::foundation;
using lasercnc::infrastructure::BsThreadPoolExecutor;
using lasercnc::infrastructure::BsThreadPoolExecutorOptions;

namespace {

std::unique_ptr<BsThreadPoolExecutor> makeExecutor(std::size_t threads = 2U)
{
    auto created = BsThreadPoolExecutor::create(BsThreadPoolExecutorOptions {threads});
    REQUIRE(created.hasValue());
    return std::move(created).value();
}

} // namespace

TEST_CASE("BsThreadPoolExecutor runs work and completion exactly once", "[infrastructure][executor]")
{
    auto executor = makeExecutor();
    CHECK(executor->concurrency() == 2U);

    std::atomic<int> workCount {0};
    std::atomic<int> completionCount {0};
    std::atomic<int> failures {0};
    for(int index = 0; index < 32; ++index) {
        auto submitted = executor->submit(
            [&workCount]() {
                workCount.fetch_add(1);
                return Result<void>::success();
            },
            [&completionCount, &failures](Result<void> result) {
                completionCount.fetch_add(1);
                if(!result.hasValue()) {
                    failures.fetch_add(1);
                }
            });
        REQUIRE(submitted.hasValue());
    }

    REQUIRE(executor->waitIdle().hasValue());
    CHECK(workCount.load() == 32);
    CHECK(completionCount.load() == 32);
    CHECK(failures.load() == 0);
}

TEST_CASE("BsThreadPoolExecutor forwards failures and contains exceptions", "[infrastructure][executor]")
{
    auto executor = makeExecutor(1U);
    std::mutex mutex;
    std::vector<std::string> observedCodes;

    REQUIRE(executor
                ->submit(
                    []() {
                        return Result<void>::failure(makeError(
                            "Task.ExpectedFailure", ErrorCategory::Validation, "expected"));
                    },
                    [&mutex, &observedCodes](Result<void> result) {
                        std::lock_guard lock(mutex);
                        observedCodes.emplace_back(result.error().code.value());
                    })
                .hasValue());
    REQUIRE(executor
                ->submit(
                    []() -> Result<void> { throw std::runtime_error("work exploded"); },
                    [&mutex, &observedCodes](Result<void> result) {
                        std::lock_guard lock(mutex);
                        observedCodes.emplace_back(result.error().code.value());
                        throw std::runtime_error("completion exploded");
                    })
                .hasValue());
    REQUIRE(executor
                ->submit(
                    []() { return Result<void>::success(); },
                    [&mutex, &observedCodes](Result<void> result) {
                        std::lock_guard lock(mutex);
                        observedCodes.emplace_back(result.hasValue() ? "success" : "unexpected-failure");
                    })
                .hasValue());

    REQUIRE(executor->waitIdle().hasValue());
    REQUIRE(observedCodes.size() == 3U);
    CHECK(observedCodes[0] == "Task.ExpectedFailure");
    CHECK(observedCodes[1] == "Execution.WorkThrew");
    CHECK(observedCodes[2] == "success");
}

TEST_CASE("BsThreadPoolExecutor shutdown drains and rejects new work", "[infrastructure][executor]")
{
    auto executor = makeExecutor();
    std::atomic<int> completions {0};
    REQUIRE(executor
                ->submit(
                    []() { return Result<void>::success(); },
                    [&completions](Result<void>) { completions.fetch_add(1); })
                .hasValue());

    REQUIRE(executor->shutdown().hasValue());
    CHECK(completions.load() == 1);
    CHECK(executor->shutdown().hasValue());

    auto rejected = executor->submit(
        []() { return Result<void>::success(); }, [](Result<void>) {});
    REQUIRE_FALSE(rejected.hasValue());
    CHECK(std::string(rejected.error().code.value()) == "Execution.ExecutorStopped");
}

TEST_CASE("BsThreadPoolExecutor validates callbacks and prevents worker deadlock", "[infrastructure][executor]")
{
    auto executor = makeExecutor(1U);
    auto noWork = executor->submit({}, [](Result<void>) {});
    REQUIRE_FALSE(noWork.hasValue());
    CHECK(std::string(noWork.error().code.value()) == "Execution.InvalidWork");

    auto noCompletion = executor->submit([]() { return Result<void>::success(); }, {});
    REQUIRE_FALSE(noCompletion.hasValue());
    CHECK(std::string(noCompletion.error().code.value()) == "Execution.InvalidCompletion");

    std::optional<std::string> waitError;
    std::optional<std::string> shutdownError;
    REQUIRE(executor
                ->submit(
                    [&executor, &waitError, &shutdownError]() {
                        auto waited = executor->waitIdle();
                        if(!waited.hasValue()) {
                            waitError = std::string(waited.error().code.value());
                        }
                        auto stopped = executor->shutdown();
                        if(!stopped.hasValue()) {
                            shutdownError = std::string(stopped.error().code.value());
                        }
                        return Result<void>::success();
                    },
                    [](Result<void>) {})
                .hasValue());
    REQUIRE(executor->waitIdle().hasValue());
    REQUIRE(waitError.has_value());
    REQUIRE(shutdownError.has_value());
    CHECK(*waitError == "Execution.WaitFromWorkerDenied");
    CHECK(*shutdownError == "Execution.ShutdownFromWorkerDenied");
}

TEST_CASE("BsThreadPoolExecutor serializes concurrent shutdown callers", "[infrastructure][executor]")
{
    auto executor = makeExecutor(2U);
    std::atomic<int> completions {0};
    for(int index = 0; index < 64; ++index) {
        REQUIRE(executor
                    ->submit(
                        []() { return Result<void>::success(); },
                        [&completions](Result<void>) { completions.fetch_add(1); })
                    .hasValue());
    }

    auto first = std::async(std::launch::async, [&executor]() {
        return executor->shutdown().hasValue();
    });
    auto second = std::async(std::launch::async, [&executor]() {
        return executor->shutdown().hasValue();
    });
    CHECK(first.get());
    CHECK(second.get());
    CHECK(completions.load() == 64);
}
