#include <lasercnc/kernel/module_runtime.hpp>

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/foundation/value.hpp>
#include <lasercnc/kernel/app_kernel.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace lasercnc::kernel {
namespace {

foundation::Value moduleDetails(const ModuleDescriptor& descriptor, std::string stage = {})
{
    foundation::Value::Object details {
        {"moduleId", foundation::Value {std::string(descriptor.id.value())}},
        {"version", foundation::Value {descriptor.version.toString()}},
    };
    if(!stage.empty()) {
        details.emplace("stage", foundation::Value {std::move(stage)});
    }
    return foundation::Value {std::move(details)};
}

foundation::Error wrapLifecycleError(
    const ModuleDescriptor& descriptor,
    std::string stage,
    foundation::Error cause)
{
    const auto category = cause.category;
    const auto severity = cause.severity;
    return foundation::makeError(
        "Kernel.ModuleLifecycleFailed",
        category,
        "A module lifecycle callback failed",
        moduleDetails(descriptor, std::move(stage)),
        severity,
        std::make_shared<const foundation::Error>(std::move(cause)));
}

template <typename Callback>
foundation::Result<void> invokeLifecycle(
    const ModuleDescriptor& descriptor,
    const char* stage,
    Callback&& callback)
{
    try {
        auto result = std::forward<Callback>(callback)();
        if(!result) {
            return foundation::Result<void>::failure(wrapLifecycleError(
                descriptor,
                stage,
                std::move(result).error()));
        }
        return foundation::Result<void>::success();
    } catch(const std::exception& exception) {
        return foundation::Result<void>::failure(wrapLifecycleError(
            descriptor,
            stage,
            foundation::makeError(
                "Kernel.ModuleLifecycleException",
                foundation::ErrorCategory::Internal,
                exception.what())));
    } catch(...) {
        return foundation::Result<void>::failure(wrapLifecycleError(
            descriptor,
            stage,
            foundation::makeError(
                "Kernel.ModuleLifecycleUnknownException",
                foundation::ErrorCategory::Internal,
                "A module lifecycle callback threw an unknown exception")));
    }
}

foundation::Result<void> runtimeStateError(const char* message)
{
    return foundation::Result<void>::failure(foundation::makeError(
        "Kernel.ModuleRuntimeStateConflict",
        foundation::ErrorCategory::Conflict,
        message));
}

foundation::Error combineBootstrapAndRollbackErrors(
    foundation::Error bootstrapError,
    foundation::Error rollbackError)
{
    return foundation::makeError(
        "Kernel.ModuleRollbackFailed",
        foundation::ErrorCategory::Internal,
        "Module bootstrap failed and rollback was not clean",
        foundation::Value {foundation::Value::Object {
            {"rollbackCode", foundation::Value {std::string(rollbackError.code.value())}},
            {"rollbackMessage", foundation::Value {rollbackError.message}},
        }},
        foundation::Severity::Error,
        std::make_shared<const foundation::Error>(std::move(bootstrapError)));
}

} // namespace

ModuleRuntime::ModuleRuntime(ServiceRegistry& services)
    : services_(services)
{
}

foundation::Result<void> ModuleRuntime::addModule(std::unique_ptr<IModule> module)
{
    if(state_ != ModuleRuntimeState::Configuring) {
        return runtimeStateError("Modules can only be added while the runtime is configuring");
    }
    if(module == nullptr) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.ModuleNull",
            foundation::ErrorCategory::Validation,
            "A module instance must not be null"));
    }

    const ModuleDescriptor descriptor = module->descriptor();
    const bool blankDisplayName = descriptor.displayName.empty()
        || std::all_of(
            descriptor.displayName.begin(),
            descriptor.displayName.end(),
            [](unsigned char character) { return std::isspace(character) != 0; });
    if(blankDisplayName) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.ModuleDisplayNameInvalid",
            foundation::ErrorCategory::Validation,
            "A module display name must not be empty",
            moduleDetails(descriptor)));
    }

    const auto duplicate = std::find_if(
        records_.begin(),
        records_.end(),
        [&descriptor](const Record& record) { return record.descriptor.id == descriptor.id; });
    if(duplicate != records_.end()) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.ModuleAlreadyRegistered",
            foundation::ErrorCategory::Conflict,
            "The module ID is already registered",
            moduleDetails(descriptor)));
    }

    records_.push_back(Record {std::move(module), descriptor});
    return foundation::Result<void>::success();
}

foundation::Result<std::vector<std::size_t>> ModuleRuntime::buildStartupOrder() const
{
    std::map<ModuleId, std::size_t> indexById;
    for(std::size_t index = 0; index < records_.size(); ++index) {
        indexById.emplace(records_[index].descriptor.id, index);
    }

    std::vector<std::size_t> dependencyCount(records_.size(), 0);
    std::vector<std::vector<std::size_t>> dependents(records_.size());

    for(std::size_t index = 0; index < records_.size(); ++index) {
        std::set<ModuleId> seenDependencies;
        for(const auto& dependency : records_[index].descriptor.dependencies) {
            if(!seenDependencies.insert(dependency.id).second) {
                return foundation::Result<std::vector<std::size_t>>::failure(
                    foundation::makeError(
                        "Kernel.ModuleDependencyDuplicated",
                        foundation::ErrorCategory::Validation,
                        "A module dependency is declared more than once",
                        moduleDetails(records_[index].descriptor)));
            }

            const auto provider = indexById.find(dependency.id);
            if(provider == indexById.end()) {
                return foundation::Result<std::vector<std::size_t>>::failure(
                    foundation::makeError(
                        "Kernel.ModuleDependencyMissing",
                        foundation::ErrorCategory::NotFound,
                        "A required module dependency is missing",
                        foundation::Value {foundation::Value::Object {
                            {"moduleId", foundation::Value {std::string(records_[index].descriptor.id.value())}},
                            {"dependencyId", foundation::Value {std::string(dependency.id.value())}},
                        }}));
            }

            const auto& providerVersion = records_[provider->second].descriptor.version;
            if(providerVersion.major != dependency.minimumVersion.major
                || providerVersion < dependency.minimumVersion) {
                return foundation::Result<std::vector<std::size_t>>::failure(
                    foundation::makeError(
                        "Kernel.ModuleDependencyVersionConflict",
                        foundation::ErrorCategory::Conflict,
                        "A module dependency version is incompatible",
                        foundation::Value {foundation::Value::Object {
                            {"moduleId", foundation::Value {std::string(records_[index].descriptor.id.value())}},
                            {"dependencyId", foundation::Value {std::string(dependency.id.value())}},
                            {"minimumVersion", foundation::Value {dependency.minimumVersion.toString()}},
                            {"actualVersion", foundation::Value {providerVersion.toString()}},
                        }}));
            }

            ++dependencyCount[index];
            dependents[provider->second].push_back(index);
        }
    }

    std::set<ModuleId> ready;
    for(std::size_t index = 0; index < dependencyCount.size(); ++index) {
        if(dependencyCount[index] == 0) {
            ready.insert(records_[index].descriptor.id);
        }
    }

    std::vector<std::size_t> order;
    order.reserve(records_.size());
    while(!ready.empty()) {
        const ModuleId nextId = *ready.begin();
        ready.erase(ready.begin());
        const std::size_t next = indexById.at(nextId);
        order.push_back(next);

        for(const std::size_t dependent : dependents[next]) {
            --dependencyCount[dependent];
            if(dependencyCount[dependent] == 0) {
                ready.insert(records_[dependent].descriptor.id);
            }
        }
    }

    if(order.size() != records_.size()) {
        return foundation::Result<std::vector<std::size_t>>::failure(foundation::makeError(
            "Kernel.ModuleDependencyCycle",
            foundation::ErrorCategory::Conflict,
            "The module dependency graph contains a cycle"));
    }

    return foundation::Result<std::vector<std::size_t>>::success(std::move(order));
}

foundation::Result<void> ModuleRuntime::validateServiceDeclarations() const
{
    std::map<ServiceId, ModuleId> providers;
    for(const auto& record : records_) {
        std::set<ServiceId> required;
        for(const auto& serviceId : record.descriptor.requiredServices) {
            if(!required.insert(serviceId).second) {
                return foundation::Result<void>::failure(foundation::makeError(
                    "Kernel.RequiredServiceDuplicated",
                    foundation::ErrorCategory::Validation,
                    "A required service is declared more than once",
                    moduleDetails(record.descriptor)));
            }
        }

        for(const auto& serviceId : record.descriptor.providedServices) {
            const auto [found, inserted] = providers.emplace(serviceId, record.descriptor.id);
            if(!inserted) {
                return foundation::Result<void>::failure(foundation::makeError(
                    "Kernel.ServiceProviderConflict",
                    foundation::ErrorCategory::Conflict,
                    "Multiple modules declare the same provided service",
                    foundation::Value {foundation::Value::Object {
                        {"serviceId", foundation::Value {std::string(serviceId.value())}},
                        {"firstProvider", foundation::Value {std::string(found->second.value())}},
                        {"secondProvider", foundation::Value {std::string(record.descriptor.id.value())}},
                    }}));
            }
        }
    }
    return foundation::Result<void>::success();
}

void ModuleRuntime::captureRegistrationDelta(
    Record& record,
    const std::vector<ServiceId>& before)
{
    const auto after = services_.snapshot().serviceIds;
    record.registeredServices.clear();
    std::set_difference(
        after.begin(),
        after.end(),
        before.begin(),
        before.end(),
        std::back_inserter(record.registeredServices));
}

foundation::Result<void> ModuleRuntime::validateRegisteredServices(const Record& record) const
{
    auto expected = record.descriptor.providedServices;
    std::sort(expected.begin(), expected.end());
    if(expected != record.registeredServices) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Kernel.ModuleServiceDeclarationMismatch",
            foundation::ErrorCategory::Validation,
            "Registered services do not match the module descriptor",
            moduleDetails(record.descriptor, "register")));
    }
    return foundation::Result<void>::success();
}

foundation::Result<void> ModuleRuntime::validateRequiredServices(const Record& record) const
{
    for(const auto& serviceId : record.descriptor.requiredServices) {
        if(!services_.contains(serviceId)) {
            return foundation::Result<void>::failure(foundation::makeError(
                "Kernel.RequiredServiceMissing",
                foundation::ErrorCategory::NotFound,
                "A module required service is not registered",
                foundation::Value {foundation::Value::Object {
                    {"moduleId", foundation::Value {std::string(record.descriptor.id.value())}},
                    {"serviceId", foundation::Value {std::string(serviceId.value())}},
                }}));
        }
    }
    return foundation::Result<void>::success();
}

foundation::Result<void> ModuleRuntime::bootstrap(AppKernel& kernel)
{
    if(state_ != ModuleRuntimeState::Configuring) {
        return runtimeStateError("The module runtime can only bootstrap once");
    }

    auto serviceValidation = validateServiceDeclarations();
    if(!serviceValidation) {
        state_ = ModuleRuntimeState::Failed;
        return serviceValidation;
    }

    auto order = buildStartupOrder();
    if(!order) {
        state_ = ModuleRuntimeState::Failed;
        return foundation::Result<void>::failure(std::move(order).error());
    }

    startupOrder_ = std::move(order).value();
    state_ = ModuleRuntimeState::Starting;

    for(const std::size_t index : startupOrder_) {
        auto& record = records_[index];
        const auto before = services_.snapshot().serviceIds;
        auto result = invokeLifecycle(
            record.descriptor,
            "register",
            [&record, this] { return record.module->registerServices(services_); });
        captureRegistrationDelta(record, before);
        if(result) {
            result = validateRegisteredServices(record);
        }
        if(!result) {
            record.lastError = result.error();
            record.state = ModuleState::Failed;
            auto error = result.error();
            auto rollbackError = rollback(kernel, index);
            if(rollbackError.has_value()) {
                error = combineBootstrapAndRollbackErrors(
                    std::move(error),
                    std::move(*rollbackError));
            }
            return foundation::Result<void>::failure(std::move(error));
        }
        record.state = ModuleState::Registered;
    }

    for(const std::size_t index : startupOrder_) {
        auto& record = records_[index];
        auto result = validateRequiredServices(record);
        if(result) {
            result = invokeLifecycle(
                record.descriptor,
                "initialize",
                [&record, &kernel] { return record.module->initialize(kernel); });
        }
        if(!result) {
            record.lastError = result.error();
            record.state = ModuleState::Failed;
            auto error = result.error();
            auto rollbackError = rollback(kernel, index);
            if(rollbackError.has_value()) {
                error = combineBootstrapAndRollbackErrors(
                    std::move(error),
                    std::move(*rollbackError));
            }
            return foundation::Result<void>::failure(std::move(error));
        }
        record.state = ModuleState::Initialized;
    }

    for(const std::size_t index : startupOrder_) {
        auto& record = records_[index];
        auto result = invokeLifecycle(
            record.descriptor,
            "start",
            [&record, &kernel] { return record.module->start(kernel); });
        if(!result) {
            record.lastError = result.error();
            record.state = ModuleState::Failed;
            auto error = result.error();
            auto rollbackError = rollback(kernel, index);
            if(rollbackError.has_value()) {
                error = combineBootstrapAndRollbackErrors(
                    std::move(error),
                    std::move(*rollbackError));
            }
            return foundation::Result<void>::failure(std::move(error));
        }
        record.state = ModuleState::Started;
    }

    for(const std::size_t index : startupOrder_) {
        records_[index].state = ModuleState::Ready;
    }
    state_ = ModuleRuntimeState::Ready;
    return foundation::Result<void>::success();
}

std::optional<foundation::Error> ModuleRuntime::rollback(
    AppKernel& kernel,
    std::optional<std::size_t> failedIndex)
{
    std::optional<foundation::Error> firstRollbackError;
    for(auto iterator = startupOrder_.rbegin(); iterator != startupOrder_.rend(); ++iterator) {
        const std::size_t index = *iterator;
        auto& record = records_[index];
        const bool needsCleanup = record.state != ModuleState::Discovered
            || !record.registeredServices.empty();
        if(!needsCleanup) {
            continue;
        }

        record.state = ModuleState::Stopping;
        auto stopResult = invokeLifecycle(
            record.descriptor,
            "rollback",
            [&record, &kernel] { return record.module->stop(kernel); });
        if(!stopResult && !record.lastError.has_value()) {
            record.lastError = stopResult.error();
        }
        if(!stopResult && !firstRollbackError.has_value()) {
            firstRollbackError = stopResult.error();
        }

        for(auto service = record.registeredServices.rbegin();
            service != record.registeredServices.rend();
            ++service) {
            services_.remove(*service);
        }
        record.registeredServices.clear();
        record.state = ModuleState::Stopped;
    }

    if(failedIndex.has_value()) {
        records_[*failedIndex].state = ModuleState::Failed;
    }
    state_ = ModuleRuntimeState::Failed;
    return firstRollbackError;
}

foundation::Result<void> ModuleRuntime::shutdown(AppKernel& kernel)
{
    if(state_ == ModuleRuntimeState::Stopped) {
        return foundation::Result<void>::success();
    }
    if(state_ == ModuleRuntimeState::Configuring) {
        state_ = ModuleRuntimeState::Stopped;
        return foundation::Result<void>::success();
    }
    if(state_ != ModuleRuntimeState::Ready) {
        return runtimeStateError("The module runtime cannot stop from its current state");
    }

    state_ = ModuleRuntimeState::Stopping;
    std::optional<foundation::Error> firstError;
    for(auto iterator = startupOrder_.rbegin(); iterator != startupOrder_.rend(); ++iterator) {
        auto& record = records_[*iterator];
        record.state = ModuleState::Stopping;
        auto result = invokeLifecycle(
            record.descriptor,
            "stop",
            [&record, &kernel] { return record.module->stop(kernel); });
        if(!result) {
            record.lastError = result.error();
            record.state = ModuleState::Failed;
            if(!firstError.has_value()) {
                firstError = result.error();
            }
        } else {
            record.state = ModuleState::Stopped;
        }
    }

    if(firstError.has_value()) {
        state_ = ModuleRuntimeState::Failed;
        return foundation::Result<void>::failure(std::move(*firstError));
    }

    state_ = ModuleRuntimeState::Stopped;
    return foundation::Result<void>::success();
}

ModuleRuntimeState ModuleRuntime::state() const noexcept
{
    return state_;
}

std::vector<ModuleSnapshot> ModuleRuntime::snapshot() const
{
    std::vector<ModuleSnapshot> result;
    result.reserve(records_.size());
    for(const auto& record : records_) {
        result.push_back(ModuleSnapshot {
            record.descriptor.id,
            record.descriptor.version,
            record.state,
            record.lastError});
    }
    return result;
}

} // namespace lasercnc::kernel
