#include <lasercnc/kernel/service_registry.hpp>

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/foundation/value.hpp>

#include <mutex>
#include <string>
#include <utility>

namespace lasercnc::kernel {
namespace {

foundation::Value serviceDetails(const ServiceId& id)
{
    return foundation::Value {foundation::Value::Object {
        {"serviceId", foundation::Value {std::string(id.value())}},
    }};
}

} // namespace

foundation::Result<void> ServiceRegistry::registerErased(
    ServiceId id,
    std::shared_ptr<void> service,
    std::type_index type)
{
    if(service == nullptr) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.ServiceNull",
            foundation::ErrorCategory::Validation,
            "A service instance must not be null",
            serviceDetails(id)));
    }

    std::unique_lock lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.ServiceRegistryFrozen",
            foundation::ErrorCategory::Conflict,
            "The service registry is frozen",
            serviceDetails(id)));
    }

    if(entries_.contains(id)) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.ServiceAlreadyRegistered",
            foundation::ErrorCategory::Conflict,
            "The service ID is already registered",
            serviceDetails(id)));
    }

    entries_.emplace(std::move(id), Entry {std::move(service), type});
    return foundation::Result<void>::success();
}

foundation::Result<std::shared_ptr<void>> ServiceRegistry::resolveErased(
    const ServiceId& id,
    std::type_index expectedType) const
{
    std::shared_lock lock(mutex_);
    const auto found = entries_.find(id);
    if(found == entries_.end()) {
        return foundation::Result<std::shared_ptr<void>>::failure(foundation::makeError(
            "Kernel.ServiceNotFound",
            foundation::ErrorCategory::NotFound,
            "The requested service is not registered",
            serviceDetails(id)));
    }

    if(found->second.type != expectedType) {
        return foundation::Result<std::shared_ptr<void>>::failure(foundation::makeError(
            "Kernel.ServiceTypeMismatch",
            foundation::ErrorCategory::Validation,
            "The requested service type does not match the registered type",
            serviceDetails(id)));
    }

    return foundation::Result<std::shared_ptr<void>>::success(found->second.service);
}

bool ServiceRegistry::contains(const ServiceId& id) const
{
    std::shared_lock lock(mutex_);
    return entries_.contains(id);
}

bool ServiceRegistry::frozen() const
{
    std::shared_lock lock(mutex_);
    return frozen_;
}

ServiceRegistrySnapshot ServiceRegistry::snapshot() const
{
    std::shared_lock lock(mutex_);
    ServiceRegistrySnapshot result;
    result.frozen = frozen_;
    result.serviceIds.reserve(entries_.size());
    for(const auto& [id, entry] : entries_) {
        static_cast<void>(entry);
        result.serviceIds.push_back(id);
    }
    return result;
}

void ServiceRegistry::freeze()
{
    std::unique_lock lock(mutex_);
    frozen_ = true;
}

void ServiceRegistry::remove(const ServiceId& id)
{
    std::unique_lock lock(mutex_);
    if(!frozen_) {
        entries_.erase(id);
    }
}

} // namespace lasercnc::kernel
