#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/runtime/command.hpp>

#include <map>
#include <memory>
#include <shared_mutex>
#include <vector>

namespace lasercnc::kernel {
class AppKernel;
}

namespace lasercnc::runtime {

class CommandRuntime;

class CommandRegistry final {
public:
    [[nodiscard]] foundation::Result<void> registerHandler(
        CommandDescriptor descriptor,
        std::shared_ptr<ICommandHandler> handler);
    [[nodiscard]] foundation::Result<CommandDescriptor> descriptor(
        const kernel::CommandName& name) const;
    [[nodiscard]] std::vector<CommandDescriptor> descriptors() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool frozen() const;

private:
    friend class CommandRuntime;
    friend class kernel::AppKernel;

    struct Entry final {
        CommandDescriptor descriptor;
        std::shared_ptr<ICommandHandler> handler;
    };

    [[nodiscard]] foundation::Result<Entry> resolve(const kernel::CommandName& name) const;
    void freeze();

    mutable std::shared_mutex mutex_;
    std::map<kernel::CommandName, Entry> entries_;
    bool frozen_{false};
};

} // namespace lasercnc::runtime
