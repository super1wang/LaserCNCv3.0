#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/runtime/task.hpp>

#include <map>
#include <memory>
#include <shared_mutex>
#include <vector>

namespace lasercnc::kernel {
class AppKernel;
}

namespace lasercnc::runtime {

class TaskRuntime;

class TaskRegistry final {
public:
    [[nodiscard]] foundation::Result<void> registerHandler(
        TaskDescriptor descriptor,
        std::shared_ptr<ITaskHandler> handler);
    [[nodiscard]] foundation::Result<TaskDescriptor> descriptor(
        const kernel::TaskName& name) const;
    [[nodiscard]] std::vector<TaskDescriptor> descriptors() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool frozen() const;

private:
    friend class TaskRuntime;
    friend class kernel::AppKernel;

    struct Entry final {
        TaskDescriptor descriptor;
        std::shared_ptr<ITaskHandler> handler;
    };

    [[nodiscard]] foundation::Result<Entry> resolve(const kernel::TaskName& name) const;
    void freeze();

    mutable std::shared_mutex mutex_;
    std::map<kernel::TaskName, Entry> entries_;
    bool frozen_{false};
};

} // namespace lasercnc::runtime
