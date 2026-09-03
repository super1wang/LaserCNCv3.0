#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/runtime/task.hpp>

#include <cstddef>
#include <map>
#include <mutex>
#include <vector>

namespace lasercnc::runtime {

class EffectExecutor;

struct ResourceAvailability final {
    ResourceKind kind{ResourceKind::CPU};
    kernel::ResourceId resource;
    std::size_t capacity{0U};
    std::size_t sharedUnits{0U};
    bool exclusivelyHeld{false};
};

class ResourceManager final {
public:
    [[nodiscard]] foundation::Result<void> configure(
        ResourceKind kind,
        kernel::ResourceId resource,
        std::size_t capacity);
    void freeze();
    [[nodiscard]] bool frozen() const;
    [[nodiscard]] std::vector<ResourceAvailability> snapshot() const;

private:
    friend class EffectExecutor;
    friend class Scheduler;

    struct Key final {
        ResourceKind kind;
        kernel::ResourceId resource;
        friend auto operator<=>(const Key&, const Key&) = default;
    };

    struct Slot final {
        std::size_t capacity{1U};
        std::size_t sharedUnits{0U};
        bool exclusivelyHeld{false};
    };

    [[nodiscard]] foundation::Result<bool> tryAcquire(
        const std::vector<ResourceClaim>& claims);
    void release(const std::vector<ResourceClaim>& claims) noexcept;

    mutable std::mutex mutex_;
    std::map<Key, Slot> slots_;
    bool frozen_{false};
};

} // namespace lasercnc::runtime
