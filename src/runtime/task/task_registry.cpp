#include <lasercnc/runtime/task_registry.hpp>

#include <lasercnc/foundation/error.hpp>

#include <mutex>
#include <string>
#include <utility>

namespace lasercnc::runtime {
namespace {

foundation::Error registryError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const kernel::TaskName& name)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {foundation::Value::Object {
            {"task", foundation::Value {std::string(name.value())}},
        }});
}

} // namespace

foundation::Result<void> TaskRegistry::registerHandler(
    TaskDescriptor descriptor,
    std::shared_ptr<ITaskHandler> handler)
{
    if(handler == nullptr) {
        return foundation::Result<void>::failure(registryError(
            "Task.InvalidHandler",
            foundation::ErrorCategory::Validation,
            "A task handler is required",
            descriptor.name));
    }

    std::unique_lock lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::failure(registryError(
            "Task.RegistryFrozen",
            foundation::ErrorCategory::Conflict,
            "Task registration is closed",
            descriptor.name));
    }
    const auto name = descriptor.name;
    const auto [unused, inserted] = entries_.emplace(
        name, Entry {std::move(descriptor), std::move(handler)});
    static_cast<void>(unused);
    if(!inserted) {
        return foundation::Result<void>::failure(registryError(
            "Task.AlreadyRegistered",
            foundation::ErrorCategory::Conflict,
            "A task with the same stable name is already registered",
            name));
    }
    return foundation::Result<void>::success();
}

foundation::Result<TaskDescriptor> TaskRegistry::descriptor(const kernel::TaskName& name) const
{
    auto entry = resolve(name);
    if(!entry) {
        return foundation::Result<TaskDescriptor>::failure(std::move(entry).error());
    }
    return foundation::Result<TaskDescriptor>::success(std::move(entry).value().descriptor);
}

std::vector<TaskDescriptor> TaskRegistry::descriptors() const
{
    std::shared_lock lock(mutex_);
    std::vector<TaskDescriptor> result;
    result.reserve(entries_.size());
    for(const auto& [unused, entry] : entries_) {
        static_cast<void>(unused);
        result.push_back(entry.descriptor);
    }
    return result;
}

std::size_t TaskRegistry::size() const
{
    std::shared_lock lock(mutex_);
    return entries_.size();
}

bool TaskRegistry::frozen() const
{
    std::shared_lock lock(mutex_);
    return frozen_;
}

foundation::Result<TaskRegistry::Entry> TaskRegistry::resolve(
    const kernel::TaskName& name) const
{
    std::shared_lock lock(mutex_);
    const auto entry = entries_.find(name);
    if(entry == entries_.end()) {
        return foundation::Result<Entry>::failure(registryError(
            "Task.NotFound",
            foundation::ErrorCategory::NotFound,
            "The task is not registered",
            name));
    }
    return foundation::Result<Entry>::success(entry->second);
}

void TaskRegistry::freeze()
{
    std::unique_lock lock(mutex_);
    frozen_ = true;
}

void TaskRegistry::remove(const kernel::TaskName& name)
{
    std::unique_lock lock(mutex_);
    if(!frozen_) {
        entries_.erase(name);
    }
}

} // namespace lasercnc::runtime
