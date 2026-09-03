#pragma once

#include <lasercnc/foundation/result.hpp>

#include <cstddef>
#include <functional>

namespace lasercnc::platform {

using ExecutorWork = std::function<foundation::Result<void>()>;
using ExecutorCompletion = std::function<void(foundation::Result<void>)>;

class ITaskExecutor {
public:
    virtual ~ITaskExecutor() = default;
    // Failure or exception means no work/completion is retained or scheduled.
    // Success transfers work and requires exactly one completion (which may be inline).
    // 中文翻译：失败或异常表示未保留、未调度工作或回调；成功后必须恰好完成一次，可同步回调。
    [[nodiscard]] virtual foundation::Result<void> submit(
        ExecutorWork work,
        ExecutorCompletion completion) = 0;
    [[nodiscard]] virtual foundation::Result<void> waitIdle() = 0;
    [[nodiscard]] virtual foundation::Result<void> shutdown() = 0;
    [[nodiscard]] virtual std::size_t concurrency() const noexcept = 0;
};

} // namespace lasercnc::platform
