#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/runtime/command.hpp>

#include <memory>
#include <vector>

namespace lasercnc::foundation {
class ISchemaValidator;
}

namespace lasercnc::persistence {
class PersistenceService;
}

namespace lasercnc::state {
class DocumentStore;
}

namespace lasercnc::runtime {

class EffectGuardRegistry;
class ResourceManager;

struct ExternalEffectExecutionResult final {
    foundation::Value result;
    bool replayed{false};
    RecoveryDisposition disposition{RecoveryDisposition::Completed};
};

class EffectExecutor final {
public:
    EffectExecutor(
        EffectGuardRegistry& guards,
        ResourceManager& resources,
        const state::DocumentStore& documents,
        persistence::PersistenceService& persistence) noexcept;

    [[nodiscard]] foundation::Result<void> validate(
        const std::vector<CommandDescriptor>& commands) const;
    [[nodiscard]] foundation::Result<ExternalEffectExecutionResult> execute(
        const CommandRequest& request,
        const CommandDescriptor& descriptor,
        const std::shared_ptr<IExternalEffectHandler>& handler,
        const foundation::ISchemaValidator& schemaValidator);

private:
    EffectGuardRegistry& guards_;
    ResourceManager& resources_;
    const state::DocumentStore& documents_;
    persistence::PersistenceService& persistence_;
};

} // namespace lasercnc::runtime
