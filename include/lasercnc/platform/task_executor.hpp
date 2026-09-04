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
    // Success acknowledges admission closure and completion of retained work/callbacks.
    // Failure or exception is not an acknowledgement; a later shutdown may retry.
    // 中文翻译：成功确认入口关闭且已保留工作/回调全部完成；失败或异常不代表确认，后续可重试。
    [[nodiscard]] virtual foundation::Result<void> shutdown() = 0;
    // Final, idempotent lifetime barrier: close admission and finish retained work/callbacks.
    // May block without a timeout. If safety cannot be guaranteed, terminate rather than return.
    // Destruction must provide the same guarantee; never destroy from an owned worker.
    // 中文翻译：最终幂等寿命屏障，关闭准入并完成已保留工作/回调；可无期限等待。
    // 中文翻译：无法保证安全时必须终止而非返回；析构同样保证，不得从所属工作线程销毁。
    virtual void drainForDestruction() noexcept = 0;
    // Nonblocking, side-effect-free query; includes both work and completion on owned workers.
    // 中文翻译：无阻塞、无副作用查询，覆盖所属工作线程上的工作及完成回调。
    [[nodiscard]] virtual bool isCurrentWorkerThread() const noexcept = 0;
    [[nodiscard]] virtual std::size_t concurrency() const noexcept = 0;
};

} // namespace lasercnc::platform
