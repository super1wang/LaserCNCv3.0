#pragma once

#include <lasercnc/foundation/result.hpp>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <utility>

namespace lasercnc::kernel {

// One Host gate covers a complete public call, not only its inner handler.
// 中文翻译：单 Host 准入覆盖整个公开调用，而不只是内部 Handler 的运行区间。
class ExecutionAdmission final {
public:
    class Lease final {
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
        Lease& operator=(Lease&&) = delete;
        ~Lease()
        {
            if(owner_ != nullptr) {
                std::lock_guard lock(owner_->mutex_);
                --owner_->active_;
            }
        }
    private:
        explicit Lease(ExecutionAdmission* owner) noexcept : owner_(owner) {}
        ExecutionAdmission* owner_{nullptr};
        friend class ExecutionAdmission;
    };

    [[nodiscard]] static foundation::Result<Lease> acquire(ExecutionAdmission* gate, const char* errorCode)
    {
        // Standalone components have their own lifecycle; only AppKernel binds this gate.
        // 中文翻译：独立组件保持自身生命周期，只有 AppKernel 绑定此整体准入门。
        if(gate == nullptr) { return foundation::Result<Lease>::success(Lease{}); }
        std::lock_guard lock(gate->mutex_);
        if(!gate->accepting_) {
            return foundation::Result<Lease>::failure(foundation::makeError(errorCode,
                foundation::ErrorCategory::Conflict, "The application kernel is not accepting this operation"));
        }
        ++gate->active_;
        return foundation::Result<Lease>::success(Lease{gate});
    }

    [[nodiscard]] bool closeIfIdle()
    {
        std::lock_guard lock(mutex_);
        if(active_ != 0U) { return false; }
        accepting_ = false;
        return true;
    }

    void open()
    {
        std::lock_guard lock(mutex_);
        accepting_ = true;
    }

private:
    std::mutex mutex_;
    std::size_t active_{0U};
    bool accepting_{false};
};

class LifecycleCall final {
public:
    explicit LifecycleCall(std::atomic_flag& active) noexcept
        : active_(active), acquired_(!active_.test_and_set(std::memory_order_acquire)) {}
    ~LifecycleCall() { if(acquired_) { active_.clear(std::memory_order_release); } }
    LifecycleCall(const LifecycleCall&) = delete;
    LifecycleCall& operator=(const LifecycleCall&) = delete;
    [[nodiscard]] bool acquired() const noexcept { return acquired_; }
private:
    std::atomic_flag& active_;
    bool acquired_;
};

} // namespace lasercnc::kernel
