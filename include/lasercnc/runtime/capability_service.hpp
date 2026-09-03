#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/kernel/identifiers.hpp>

#include <map>
#include <set>
#include <shared_mutex>
#include <span>
#include <vector>

namespace lasercnc::runtime {

struct SessionCapabilities final {
    kernel::SessionId sessionId;
    std::vector<kernel::CapabilityId> capabilities;
};

class CapabilityService final {
public:
    [[nodiscard]] foundation::Result<void> replace(
        kernel::SessionId sessionId,
        std::span<const kernel::CapabilityId> capabilities);
    [[nodiscard]] foundation::Result<void> remove(const kernel::SessionId& sessionId);
    [[nodiscard]] foundation::Result<void> authorize(
        const kernel::SessionId& sessionId,
        const kernel::CapabilityId& capability) const;
    [[nodiscard]] SessionCapabilities snapshot(const kernel::SessionId& sessionId) const;

private:
    mutable std::shared_mutex mutex_;
    std::map<kernel::SessionId, std::set<kernel::CapabilityId>> sessions_;
};

} // namespace lasercnc::runtime
