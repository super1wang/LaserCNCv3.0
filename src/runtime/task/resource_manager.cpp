#include <lasercnc/runtime/resource_manager.hpp>

#include <lasercnc/foundation/error.hpp>

#include <limits>
#include <map>
#include <string>
#include <utility>

namespace lasercnc::runtime {
namespace {

bool validResourceKind(ResourceKind kind) noexcept
{
    switch(kind) {
    case ResourceKind::CPU:
    case ResourceKind::DiskIO:
    case ResourceKind::GPU:
    case ResourceKind::OCCT:
    case ResourceKind::ProjectRead:
    case ResourceKind::ProjectWrite:
    case ResourceKind::MachineController:
    case ResourceKind::CollisionBackend: return true;
    }
    return false;
}

bool validResourceAccess(ResourceAccess access) noexcept
{
    switch(access) {
    case ResourceAccess::Shared:
    case ResourceAccess::Exclusive: return true;
    }
    return false;
}

ResourceKind canonicalKind(ResourceKind kind) noexcept
{
    return kind == ResourceKind::ProjectWrite ? ResourceKind::ProjectRead : kind;
}

foundation::Error resourceError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const kernel::ResourceId& resource)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {foundation::Value::Object {
            {"resource", foundation::Value {std::string(resource.value())}},
        }});
}

} // namespace

foundation::Result<void> ResourceManager::configure(
    ResourceKind kind,
    kernel::ResourceId resource,
    std::size_t capacity)
{
    if(!validResourceKind(kind)) {
        return foundation::Result<void>::failure(resourceError(
            "Task.InvalidResourceKind",
            foundation::ErrorCategory::Validation,
            "The resource kind is invalid",
            resource));
    }
    if(capacity == 0U) {
        return foundation::Result<void>::failure(resourceError(
            "Task.InvalidResourceCapacity",
            foundation::ErrorCategory::Validation,
            "Resource capacity must be greater than zero",
            resource));
    }
    std::lock_guard lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::failure(resourceError(
            "Task.ResourceModelFrozen",
            foundation::ErrorCategory::Conflict,
            "Resource configuration is frozen",
            resource));
    }
    const Key key {canonicalKind(kind), resource};
    auto [slot, inserted] = slots_.try_emplace(key, Slot {capacity, 0U, false});
    if(!inserted) {
        slot->second.capacity = capacity;
    }
    return foundation::Result<void>::success();
}

void ResourceManager::freeze()
{
    std::lock_guard lock(mutex_);
    frozen_ = true;
}

bool ResourceManager::frozen() const
{
    std::lock_guard lock(mutex_);
    return frozen_;
}

std::vector<ResourceAvailability> ResourceManager::snapshot() const
{
    std::lock_guard lock(mutex_);
    std::vector<ResourceAvailability> result;
    result.reserve(slots_.size());
    for(const auto& [key, slot] : slots_) {
        result.push_back(ResourceAvailability {
            key.kind, key.resource, slot.capacity, slot.sharedUnits, slot.exclusivelyHeld});
    }
    return result;
}

foundation::Result<bool> ResourceManager::tryAcquire(
    const std::vector<ResourceClaim>& claims)
{
    struct Aggregate final {
        ResourceAccess access{ResourceAccess::Shared};
        std::size_t units{0U};
    };
    std::map<Key, Aggregate> requested;
    for(const auto& claim : claims) {
        if(!validResourceKind(claim.kind)) {
            return foundation::Result<bool>::failure(resourceError(
                "Task.InvalidResourceKind",
                foundation::ErrorCategory::Validation,
                "The resource kind is invalid",
                claim.resource));
        }
        if(!validResourceAccess(claim.access)) {
            return foundation::Result<bool>::failure(resourceError(
                "Task.InvalidResourceAccess",
                foundation::ErrorCategory::Validation,
                "The resource access mode is invalid",
                claim.resource));
        }
        if(claim.units == 0U) {
            return foundation::Result<bool>::failure(resourceError(
                "Task.InvalidResourceUnits",
                foundation::ErrorCategory::Validation,
                "Resource units must be greater than zero",
                claim.resource));
        }
        const auto access = claim.kind == ResourceKind::ProjectWrite
            ? ResourceAccess::Exclusive
            : claim.access;
        const Key key {canonicalKind(claim.kind), claim.resource};
        auto [entry, inserted] = requested.try_emplace(key, Aggregate {access, claim.units});
        if(!inserted) {
            if(entry->second.access != access || access == ResourceAccess::Exclusive) {
                return foundation::Result<bool>::failure(resourceError(
                    "Task.ConflictingResourceClaims",
                    foundation::ErrorCategory::Validation,
                    "A task contains conflicting claims for the same resource",
                    claim.resource));
            }
            if(claim.units > std::numeric_limits<std::size_t>::max() - entry->second.units) {
                return foundation::Result<bool>::failure(resourceError(
                    "Task.ResourceUnitsOverflow",
                    foundation::ErrorCategory::Validation,
                    "Aggregated resource units exceed the representable range",
                    claim.resource));
            }
            entry->second.units += claim.units;
        }
    }

    std::lock_guard lock(mutex_);
    for(const auto& [key, aggregate] : requested) {
        auto [slot, unused] = slots_.try_emplace(key, Slot {});
        static_cast<void>(unused);
        if(aggregate.access == ResourceAccess::Exclusive) {
            if(slot->second.exclusivelyHeld || slot->second.sharedUnits != 0U) {
                return foundation::Result<bool>::success(false);
            }
        } else if(slot->second.exclusivelyHeld
                  || aggregate.units > slot->second.capacity - slot->second.sharedUnits) {
            return foundation::Result<bool>::success(false);
        }
    }
    for(const auto& [key, aggregate] : requested) {
        auto& slot = slots_.at(key);
        if(aggregate.access == ResourceAccess::Exclusive) {
            slot.exclusivelyHeld = true;
        } else {
            slot.sharedUnits += aggregate.units;
        }
    }
    return foundation::Result<bool>::success(true);
}

void ResourceManager::release(const std::vector<ResourceClaim>& claims) noexcept
{
    std::lock_guard lock(mutex_);
    for(const auto& claim : claims) {
        const Key key {canonicalKind(claim.kind), claim.resource};
        const auto slot = slots_.find(key);
        if(slot == slots_.end()) {
            continue;
        }
        const auto access = claim.kind == ResourceKind::ProjectWrite
            ? ResourceAccess::Exclusive
            : claim.access;
        if(access == ResourceAccess::Exclusive) {
            slot->second.exclusivelyHeld = false;
        } else if(slot->second.sharedUnits >= claim.units) {
            slot->second.sharedUnits -= claim.units;
        } else {
            slot->second.sharedUnits = 0U;
        }
    }
}

} // namespace lasercnc::runtime
