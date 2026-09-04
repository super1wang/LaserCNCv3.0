#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/runtime/command.hpp>

#include <map>
#include <memory>
#include <shared_mutex>
#include <vector>

namespace lasercnc::kernel {
class AppKernel;
class ModuleRuntime;
}

namespace lasercnc::runtime {

class CommandRuntime;

class CommandRegistry final {
public:
    [[nodiscard]] foundation::Result<void> registerLifecycleCommand(CommandDescriptor descriptor);
    [[nodiscard]] foundation::Result<void> registerHandler(
        CommandDescriptor descriptor,
        std::shared_ptr<ICommandHandler> handler);
    [[nodiscard]] foundation::Result<void> registerAsyncHandler(
        CommandDescriptor descriptor,
        std::shared_ptr<IAsyncCommandHandler> handler);
    [[nodiscard]] foundation::Result<void> registerReadOnlyHandler(
        CommandDescriptor descriptor,
        std::shared_ptr<IReadOnlyCommandHandler> handler);
    [[nodiscard]] foundation::Result<void> registerExternalEffectHandler(
        CommandDescriptor descriptor,
        std::shared_ptr<IExternalEffectHandler> handler);
    [[nodiscard]] foundation::Result<CommandDescriptor> descriptor(
        const CommandKey& key,
        VersionResolution resolution = VersionResolution::Exact) const;
    [[nodiscard]] std::vector<CommandDescriptor> descriptors() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool frozen() const;

private:
    friend class CommandRuntime;
    friend class kernel::AppKernel;
    friend class kernel::ModuleRuntime;

    struct Entry final {
        CommandDescriptor descriptor;
        std::shared_ptr<ICommandHandler> handler;
        std::shared_ptr<IAsyncCommandHandler> asyncHandler;
        std::shared_ptr<IReadOnlyCommandHandler> readOnlyHandler;
        std::shared_ptr<IExternalEffectHandler> externalEffectHandler;
    };

    [[nodiscard]] foundation::Result<Entry> resolve(
        const CommandKey& key,
        VersionResolution resolution) const;
    void freeze();
    void remove(const CommandKey& key);

    mutable std::shared_mutex mutex_;
    std::map<CommandKey, Entry> entries_;
    bool frozen_{false};
};

} // namespace lasercnc::runtime
