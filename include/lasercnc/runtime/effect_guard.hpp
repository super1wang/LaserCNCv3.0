#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/kernel/identifiers.hpp>
#include <lasercnc/runtime/command.hpp>

#include <map>
#include <cstddef>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <vector>

namespace lasercnc::kernel {
class AppKernel;
}

namespace lasercnc::runtime {

struct EffectGuardContext final {
    std::optional<state::Document> document;
};

class IEffectGuard {
public:
    virtual ~IEffectGuard() = default;

    [[nodiscard]] virtual foundation::Result<void> evaluate(
        const CommandRequest& request,
        const CommandDescriptor& descriptor,
        const EffectGuardContext& context) = 0;
};

class EffectGuardRegistry final {
public:
    [[nodiscard]] foundation::Result<void> registerGuard(
        kernel::EffectGuardId id,
        std::shared_ptr<IEffectGuard> guard);
    [[nodiscard]] foundation::Result<std::shared_ptr<IEffectGuard>> guard(
        const kernel::EffectGuardId& id) const;
    [[nodiscard]] std::vector<kernel::EffectGuardId> ids() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool frozen() const;

private:
    friend class kernel::AppKernel;

    void freeze();

    mutable std::shared_mutex mutex_;
    std::map<kernel::EffectGuardId, std::shared_ptr<IEffectGuard>> guards_;
    bool frozen_{false};
};

} // namespace lasercnc::runtime
