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
    [[nodiscard]] virtual foundation::Result<void> submit(
        ExecutorWork work,
        ExecutorCompletion completion) = 0;
    [[nodiscard]] virtual foundation::Result<void> waitIdle() = 0;
    [[nodiscard]] virtual foundation::Result<void> shutdown() = 0;
    [[nodiscard]] virtual std::size_t concurrency() const noexcept = 0;
};

} // namespace lasercnc::platform
