#include <lasercnc/runtime/execution_services.hpp>

#include <lasercnc/foundation/error.hpp>

#include <mutex>
#include <utility>

namespace lasercnc::runtime {

foundation::Result<void> ExecutionServices::configure(
    std::shared_ptr<foundation::ISchemaValidator> schemaValidator,
    std::shared_ptr<observability::ILogService> logService)
{
    if(schemaValidator == nullptr || logService == nullptr) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Runtime.InvalidExecutionServices",
            foundation::ErrorCategory::Validation,
            "Schema validation and logging services are both required"));
    }
    std::unique_lock lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Runtime.ExecutionServicesFrozen",
            foundation::ErrorCategory::Conflict,
            "Execution services cannot change after the application kernel is ready"));
    }
    schemaValidator_ = std::move(schemaValidator);
    logService_ = std::move(logService);
    return foundation::Result<void>::success();
}

foundation::Result<ExecutionServicesSnapshot> ExecutionServices::snapshot() const
{
    std::shared_lock lock(mutex_);
    if(schemaValidator_ == nullptr || logService_ == nullptr) {
        return foundation::Result<ExecutionServicesSnapshot>::failure(foundation::makeError(
            "Runtime.ExecutionServicesNotConfigured",
            foundation::ErrorCategory::Conflict,
            "Execution services have not been configured"));
    }
    return foundation::Result<ExecutionServicesSnapshot>::success(
        ExecutionServicesSnapshot {schemaValidator_, logService_});
}

bool ExecutionServices::configured() const
{
    std::shared_lock lock(mutex_);
    return schemaValidator_ != nullptr && logService_ != nullptr;
}

bool ExecutionServices::frozen() const
{
    std::shared_lock lock(mutex_);
    return frozen_;
}

void ExecutionServices::freeze()
{
    std::unique_lock lock(mutex_);
    frozen_ = true;
}

} // namespace lasercnc::runtime
