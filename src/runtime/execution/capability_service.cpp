#include <lasercnc/runtime/capability_service.hpp>

#include <lasercnc/foundation/error.hpp>

#include <mutex>
#include <string>
#include <utility>

namespace lasercnc::runtime {

foundation::Result<void> CapabilityService::replace(
    kernel::SessionId sessionId,
    std::span<const kernel::CapabilityId> capabilities)
{
    std::set<kernel::CapabilityId> replacement(capabilities.begin(), capabilities.end());
    std::unique_lock lock(mutex_);
    sessions_.insert_or_assign(std::move(sessionId), std::move(replacement));
    return foundation::Result<void>::success();
}

foundation::Result<void> CapabilityService::remove(const kernel::SessionId& sessionId)
{
    std::unique_lock lock(mutex_);
    if(sessions_.erase(sessionId) == 0U) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Capability.SessionNotFound",
            foundation::ErrorCategory::NotFound,
            "The capability session was not found",
            foundation::Value {foundation::Value::Object {
                {"sessionId", foundation::Value {std::string(sessionId.value())}},
            }}));
    }
    return foundation::Result<void>::success();
}

foundation::Result<void> CapabilityService::authorize(
    const kernel::SessionId& sessionId,
    const kernel::CapabilityId& capability) const
{
    std::shared_lock lock(mutex_);
    const auto session = sessions_.find(sessionId);
    if(session != sessions_.end() && session->second.contains(capability)) {
        return foundation::Result<void>::success();
    }
    return foundation::Result<void>::failure(foundation::makeError(
        "Capability.Denied",
        foundation::ErrorCategory::Authorization,
        "The session does not own the required capability",
        foundation::Value {foundation::Value::Object {
            {"sessionId", foundation::Value {std::string(sessionId.value())}},
            {"capability", foundation::Value {std::string(capability.value())}},
        }}));
}

SessionCapabilities CapabilityService::snapshot(const kernel::SessionId& sessionId) const
{
    std::shared_lock lock(mutex_);
    SessionCapabilities result {sessionId, {}};
    const auto session = sessions_.find(sessionId);
    if(session != sessions_.end()) {
        result.capabilities.assign(session->second.begin(), session->second.end());
    }
    return result;
}

} // namespace lasercnc::runtime
