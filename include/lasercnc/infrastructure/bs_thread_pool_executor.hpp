#pragma once

#include <lasercnc/platform/task_executor.hpp>

#include <cstddef>
#include <memory>

namespace lasercnc::infrastructure {

struct BsThreadPoolExecutorOptions final {
    std::size_t threadCount{0U};
};

class BsThreadPoolExecutor final : public platform::ITaskExecutor {
public:
    [[nodiscard]] static foundation::Result<std::unique_ptr<BsThreadPoolExecutor>> create(
        BsThreadPoolExecutorOptions options = {});

    ~BsThreadPoolExecutor() override;

    BsThreadPoolExecutor(const BsThreadPoolExecutor&) = delete;
    BsThreadPoolExecutor& operator=(const BsThreadPoolExecutor&) = delete;
    BsThreadPoolExecutor(BsThreadPoolExecutor&&) = delete;
    BsThreadPoolExecutor& operator=(BsThreadPoolExecutor&&) = delete;

    [[nodiscard]] foundation::Result<void> submit(
        platform::ExecutorWork work,
        platform::ExecutorCompletion completion) override;
    [[nodiscard]] foundation::Result<void> waitIdle() override;
    [[nodiscard]] foundation::Result<void> shutdown() override;
    [[nodiscard]] std::size_t concurrency() const noexcept override;

private:
    class Impl;

    explicit BsThreadPoolExecutor(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace lasercnc::infrastructure
