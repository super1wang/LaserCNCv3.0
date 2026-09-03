#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/kernel/identifiers.hpp>

#include <cstddef>
#include <map>
#include <memory>
#include <shared_mutex>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

namespace lasercnc::kernel {

struct ServiceRegistrySnapshot final {
    bool frozen{false};
    std::vector<ServiceId> serviceIds;
};

class ModuleRuntime;

class ServiceRegistry final {
public:
    ServiceRegistry() = default;

    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;

    template <typename Service>
    [[nodiscard]] foundation::Result<void> registerService(
        ServiceId id,
        std::shared_ptr<Service> service)
    {
        static_assert(!std::is_void_v<Service>);
        static_assert(!std::is_const_v<Service>);
        return registerErased(
            std::move(id),
            std::static_pointer_cast<void>(std::move(service)),
            std::type_index(typeid(Service)));
    }

    template <typename Service>
    [[nodiscard]] foundation::Result<std::shared_ptr<Service>> resolve(
        const ServiceId& id) const
    {
        static_assert(!std::is_void_v<Service>);
        static_assert(!std::is_const_v<Service>);

        auto resolved = resolveErased(id, std::type_index(typeid(Service)));
        if(!resolved) {
            return foundation::Result<std::shared_ptr<Service>>::failure(
                std::move(resolved).error());
        }

        return foundation::Result<std::shared_ptr<Service>>::success(
            std::static_pointer_cast<Service>(std::move(resolved).value()));
    }

    [[nodiscard]] bool contains(const ServiceId& id) const;
    [[nodiscard]] bool frozen() const;
    [[nodiscard]] ServiceRegistrySnapshot snapshot() const;

    void freeze();

private:
    struct Entry final {
        std::shared_ptr<void> service;
        std::type_index type;
    };

    [[nodiscard]] foundation::Result<void> registerErased(
        ServiceId id,
        std::shared_ptr<void> service,
        std::type_index type);
    [[nodiscard]] foundation::Result<std::shared_ptr<void>> resolveErased(
        const ServiceId& id,
        std::type_index expectedType) const;
    void remove(const ServiceId& id);

    friend class ModuleRuntime;

    mutable std::shared_mutex mutex_;
    std::map<ServiceId, Entry> entries_;
    bool frozen_{false};
};

} // namespace lasercnc::kernel
