#include <lasercnc/runtime/effect_guard.hpp>

#include <lasercnc/foundation/error.hpp>

#include <mutex>
#include <string>
#include <utility>

namespace lasercnc::runtime {
namespace {

foundation::Error guardError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const kernel::EffectGuardId& id)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {foundation::Value::Object {
            {"guardId", foundation::Value {std::string(id.value())}},
        }});
}

} // namespace

foundation::Result<void> EffectGuardRegistry::registerGuard(
    kernel::EffectGuardId id,
    std::shared_ptr<IEffectGuard> guard)
{
    if(guard == nullptr) {
        return foundation::Result<void>::failure(guardError(
            "EffectGuard.InvalidHandler",
            foundation::ErrorCategory::Validation,
            "An effect guard implementation is required",
            id));
    }
    std::unique_lock lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::failure(guardError(
            "EffectGuard.RegistryFrozen",
            foundation::ErrorCategory::Conflict,
            "Effect guard registration is closed",
            id));
    }
    const auto [unused, inserted] = guards_.emplace(std::move(id), std::move(guard));
    static_cast<void>(unused);
    if(!inserted) {
        return foundation::Result<void>::failure(guardError(
            "EffectGuard.AlreadyRegistered",
            foundation::ErrorCategory::Conflict,
            "The effect guard identity is already registered",
            unused->first));
    }
    return foundation::Result<void>::success();
}

foundation::Result<std::shared_ptr<IEffectGuard>> EffectGuardRegistry::guard(
    const kernel::EffectGuardId& id) const
{
    std::shared_lock lock(mutex_);
    const auto found = guards_.find(id);
    if(found == guards_.end()) {
        return foundation::Result<std::shared_ptr<IEffectGuard>>::failure(guardError(
            "EffectGuard.NotFound",
            foundation::ErrorCategory::NotFound,
            "The required effect guard is not registered",
            id));
    }
    return foundation::Result<std::shared_ptr<IEffectGuard>>::success(found->second);
}

std::vector<kernel::EffectGuardId> EffectGuardRegistry::ids() const
{
    std::shared_lock lock(mutex_);
    std::vector<kernel::EffectGuardId> result;
    result.reserve(guards_.size());
    for(const auto& [id, unused] : guards_) {
        static_cast<void>(unused);
        result.push_back(id);
    }
    return result;
}

std::size_t EffectGuardRegistry::size() const
{
    std::shared_lock lock(mutex_);
    return guards_.size();
}

bool EffectGuardRegistry::frozen() const
{
    std::shared_lock lock(mutex_);
    return frozen_;
}

void EffectGuardRegistry::freeze()
{
    std::unique_lock lock(mutex_);
    frozen_ = true;
}

} // namespace lasercnc::runtime
