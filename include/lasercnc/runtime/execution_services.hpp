#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/schema.hpp>
#include <lasercnc/observability/log_service.hpp>

#include <memory>
#include <shared_mutex>

namespace lasercnc::kernel {
class AppKernel;
}

namespace lasercnc::runtime {

struct ExecutionServicesSnapshot final {
    std::shared_ptr<foundation::ISchemaValidator> schemaValidator;
    std::shared_ptr<observability::ILogService> logService;
};

class ExecutionServices final {
public:
    [[nodiscard]] foundation::Result<void> configure(
        std::shared_ptr<foundation::ISchemaValidator> schemaValidator,
        std::shared_ptr<observability::ILogService> logService);
    [[nodiscard]] foundation::Result<ExecutionServicesSnapshot> snapshot() const;
    [[nodiscard]] bool configured() const;
    [[nodiscard]] bool frozen() const;

private:
    friend class kernel::AppKernel;

    void freeze();

    mutable std::shared_mutex mutex_;
    std::shared_ptr<foundation::ISchemaValidator> schemaValidator_;
    std::shared_ptr<observability::ILogService> logService_;
    bool frozen_{false};
};

} // namespace lasercnc::runtime
