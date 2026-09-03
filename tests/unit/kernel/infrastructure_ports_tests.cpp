#include <lasercnc/observability/log_service.hpp>
#include <lasercnc/platform/config_serializer.hpp>
#include <lasercnc/platform/persistence_backend.hpp>
#include <lasercnc/platform/task_executor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

using namespace lasercnc;

namespace {

class InlineExecutor final : public platform::ITaskExecutor {
public:
    [[nodiscard]] foundation::Result<void> submit(
        platform::ExecutorWork work,
        platform::ExecutorCompletion completion) override
    {
        if(!work || !completion) {
            return foundation::Result<void>::failure(foundation::makeError(
                "Test.InvalidExecutorRequest",
                foundation::ErrorCategory::Validation,
                "Work and completion are required"));
        }
        completion(work());
        return foundation::Result<void>::success();
    }

    [[nodiscard]] foundation::Result<void> waitIdle() override
    {
        return foundation::Result<void>::success();
    }

    [[nodiscard]] foundation::Result<void> shutdown() override
    {
        return foundation::Result<void>::success();
    }

    [[nodiscard]] std::size_t concurrency() const noexcept override
    {
        return 1;
    }
};

} // namespace

TEST_CASE("Infrastructure ports are replaceable without third-party types", "[kernel][ports]")
{
    InlineExecutor executor;
    bool completed = false;
    auto submitted = executor.submit(
        [] { return foundation::Result<void>::success(); },
        [&completed](foundation::Result<void> result) { completed = result.hasValue(); });

    REQUIRE(submitted.hasValue());
    CHECK(completed);
    CHECK(executor.concurrency() == 1);
}
