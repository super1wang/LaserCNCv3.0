#include <lasercnc/runtime/command_registry.hpp>

#include <lasercnc/foundation/error.hpp>

#include <mutex>
#include <string>
#include <utility>

namespace lasercnc::runtime {
namespace {

foundation::Error commandError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const kernel::CommandName& name)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {foundation::Value::Object {
            {"command", foundation::Value {std::string(name.value())}},
        }});
}

} // namespace

foundation::Result<void> CommandRegistry::registerHandler(
    CommandDescriptor descriptor,
    std::shared_ptr<ICommandHandler> handler)
{
    if(handler == nullptr) {
        return foundation::Result<void>::failure(commandError(
            "Command.InvalidHandler",
            foundation::ErrorCategory::Validation,
            "A command handler is required",
            descriptor.name));
    }
    if(descriptor.executionMode != ExecutionMode::Synchronous) {
        return foundation::Result<void>::failure(commandError(
            "Command.HandlerModeMismatch",
            foundation::ErrorCategory::Validation,
            "A synchronous command requires an ICommandHandler",
            descriptor.name));
    }
    if(descriptor.sideEffect != SideEffectLevel::DocumentWrite) {
        return foundation::Result<void>::failure(commandError(
            "Command.SideEffectUnsupported",
            foundation::ErrorCategory::Validation,
            "Phase 5 command handlers must use the document transaction boundary",
            descriptor.name));
    }
    if(descriptor.undoable) {
        return foundation::Result<void>::failure(commandError(
            "Command.UndoUnsupported",
            foundation::ErrorCategory::Validation,
            "Undoable commands require the Phase 8 journal contract",
            descriptor.name));
    }

    std::unique_lock lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::failure(commandError(
            "Command.RegistryFrozen",
            foundation::ErrorCategory::Conflict,
            "Command registration is closed",
            descriptor.name));
    }
    const auto name = descriptor.name;
    const auto [unused, inserted] = entries_.emplace(
        name, Entry {std::move(descriptor), std::move(handler), nullptr});
    static_cast<void>(unused);
    if(!inserted) {
        return foundation::Result<void>::failure(commandError(
            "Command.AlreadyRegistered",
            foundation::ErrorCategory::Conflict,
            "A command with the same stable name is already registered",
            name));
    }
    return foundation::Result<void>::success();
}

foundation::Result<void> CommandRegistry::registerAsyncHandler(
    CommandDescriptor descriptor,
    std::shared_ptr<IAsyncCommandHandler> handler)
{
    if(handler == nullptr) {
        return foundation::Result<void>::failure(commandError(
            "Command.InvalidHandler",
            foundation::ErrorCategory::Validation,
            "An asynchronous command handler is required",
            descriptor.name));
    }
    if(descriptor.executionMode != ExecutionMode::Asynchronous) {
        return foundation::Result<void>::failure(commandError(
            "Command.HandlerModeMismatch",
            foundation::ErrorCategory::Validation,
            "An asynchronous command requires an IAsyncCommandHandler",
            descriptor.name));
    }
    if(descriptor.sideEffect != SideEffectLevel::ReadOnly) {
        return foundation::Result<void>::failure(commandError(
            "Command.AsyncSideEffectUnsupported",
            foundation::ErrorCategory::Validation,
            "Asynchronous commands may only prepare read-only background computation",
            descriptor.name));
    }
    if(descriptor.undoable) {
        return foundation::Result<void>::failure(commandError(
            "Command.UndoUnsupported",
            foundation::ErrorCategory::Validation,
            "Undoable commands require the Phase 8 journal contract",
            descriptor.name));
    }

    std::unique_lock lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::failure(commandError(
            "Command.RegistryFrozen",
            foundation::ErrorCategory::Conflict,
            "Command registration is closed",
            descriptor.name));
    }
    const auto name = descriptor.name;
    const auto [unused, inserted] = entries_.emplace(
        name, Entry {std::move(descriptor), nullptr, std::move(handler)});
    static_cast<void>(unused);
    if(!inserted) {
        return foundation::Result<void>::failure(commandError(
            "Command.AlreadyRegistered",
            foundation::ErrorCategory::Conflict,
            "A command with the same stable name is already registered",
            name));
    }
    return foundation::Result<void>::success();
}

foundation::Result<CommandDescriptor> CommandRegistry::descriptor(
    const kernel::CommandName& name) const
{
    auto entry = resolve(name);
    if(!entry.hasValue()) {
        return foundation::Result<CommandDescriptor>::failure(std::move(entry).error());
    }
    return foundation::Result<CommandDescriptor>::success(std::move(entry).value().descriptor);
}

std::vector<CommandDescriptor> CommandRegistry::descriptors() const
{
    std::shared_lock lock(mutex_);
    std::vector<CommandDescriptor> result;
    result.reserve(entries_.size());
    for(const auto& [unused, entry] : entries_) {
        static_cast<void>(unused);
        result.push_back(entry.descriptor);
    }
    return result;
}

std::size_t CommandRegistry::size() const
{
    std::shared_lock lock(mutex_);
    return entries_.size();
}

bool CommandRegistry::frozen() const
{
    std::shared_lock lock(mutex_);
    return frozen_;
}

foundation::Result<CommandRegistry::Entry> CommandRegistry::resolve(
    const kernel::CommandName& name) const
{
    std::shared_lock lock(mutex_);
    const auto entry = entries_.find(name);
    if(entry == entries_.end()) {
        return foundation::Result<Entry>::failure(commandError(
            "Command.NotFound",
            foundation::ErrorCategory::NotFound,
            "The command is not registered",
            name));
    }
    return foundation::Result<Entry>::success(entry->second);
}

void CommandRegistry::freeze()
{
    std::unique_lock lock(mutex_);
    frozen_ = true;
}

} // namespace lasercnc::runtime
