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
    const CommandKey& key)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {foundation::Value::Object {
            {"command", foundation::Value {std::string(key.name.value())}},
            {"version", foundation::Value {key.version.toString()}},
        }});
}

CommandKey keyOf(const CommandDescriptor& descriptor)
{
    return CommandKey {descriptor.name, descriptor.version};
}

bool validExternalSideEffect(SideEffectLevel sideEffect) noexcept
{
    switch(sideEffect) {
    case SideEffectLevel::FileSystemWrite:
    case SideEffectLevel::Publish:
    case SideEffectLevel::MachineControl:
    case SideEffectLevel::Motion:
    case SideEffectLevel::LaserControl:
        return true;
    case SideEffectLevel::ReadOnly:
    case SideEffectLevel::DocumentWrite:
        return false;
    }
    return false;
}

bool hasExternalMetadata(const CommandDescriptor& descriptor) noexcept
{
    return descriptor.replayPolicy != ReplayPolicy::Never
        || !descriptor.effectGuards.empty() || !descriptor.resources.empty();
}

} // namespace

foundation::Result<void> CommandRegistry::registerHandler(
    CommandDescriptor descriptor,
    std::shared_ptr<ICommandHandler> handler)
{
    const auto key = keyOf(descriptor);
    if(handler == nullptr) {
        return foundation::Result<void>::failure(commandError(
            "Command.InvalidHandler",
            foundation::ErrorCategory::Validation,
            "A command handler is required",
            key));
    }
    if(descriptor.executionMode != ExecutionMode::Synchronous) {
        return foundation::Result<void>::failure(commandError(
            "Command.HandlerModeMismatch",
            foundation::ErrorCategory::Validation,
            "A synchronous command requires an ICommandHandler",
            key));
    }
    if(!validExecutionScope(descriptor.scope)) {
        return foundation::Result<void>::failure(commandError(
            "Command.InvalidScope",
            foundation::ErrorCategory::Validation,
            "The command scope is invalid",
            key));
    }
    if(descriptor.sideEffect != SideEffectLevel::DocumentWrite) {
        return foundation::Result<void>::failure(commandError(
            "Command.SideEffectUnsupported",
            foundation::ErrorCategory::Validation,
            "Phase 5 command handlers must use the document transaction boundary",
            key));
    }
    if(descriptor.scope != ExecutionScope::Document) {
        return foundation::Result<void>::failure(commandError(
            "Command.ScopeSideEffectMismatch",
            foundation::ErrorCategory::Validation,
            "Document-write commands require document scope",
            key));
    }
    if(hasExternalMetadata(descriptor)) {
        return foundation::Result<void>::failure(commandError(
            "Command.ExternalMetadataUnsupported",
            foundation::ErrorCategory::Validation,
            "Document-write commands cannot declare external-effect metadata",
            key));
    }

    std::unique_lock lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::failure(commandError(
            "Command.RegistryFrozen",
            foundation::ErrorCategory::Conflict,
            "Command registration is closed",
            key));
    }
    const auto [unused, inserted] = entries_.emplace(
        key, Entry {std::move(descriptor), std::move(handler), nullptr});
    static_cast<void>(unused);
    if(!inserted) {
        return foundation::Result<void>::failure(commandError(
            "Command.AlreadyRegistered",
            foundation::ErrorCategory::Conflict,
            "The exact command name and version are already registered",
            key));
    }
    return foundation::Result<void>::success();
}

foundation::Result<void> CommandRegistry::registerAsyncHandler(
    CommandDescriptor descriptor,
    std::shared_ptr<IAsyncCommandHandler> handler)
{
    const auto key = keyOf(descriptor);
    if(handler == nullptr) {
        return foundation::Result<void>::failure(commandError(
            "Command.InvalidHandler",
            foundation::ErrorCategory::Validation,
            "An asynchronous command handler is required",
            key));
    }
    if(descriptor.executionMode != ExecutionMode::Asynchronous) {
        return foundation::Result<void>::failure(commandError(
            "Command.HandlerModeMismatch",
            foundation::ErrorCategory::Validation,
            "An asynchronous command requires an IAsyncCommandHandler",
            key));
    }
    if(!validExecutionScope(descriptor.scope)) {
        return foundation::Result<void>::failure(commandError(
            "Command.InvalidScope",
            foundation::ErrorCategory::Validation,
            "The command scope is invalid",
            key));
    }
    if(descriptor.sideEffect != SideEffectLevel::ReadOnly) {
        return foundation::Result<void>::failure(commandError(
            "Command.AsyncSideEffectUnsupported",
            foundation::ErrorCategory::Validation,
            "Asynchronous commands may only prepare read-only background computation",
            key));
    }
    if(descriptor.undoable) {
        return foundation::Result<void>::failure(commandError(
            "Command.UndoUnsupported",
            foundation::ErrorCategory::Validation,
            "Undoable commands require the Phase 8 journal contract",
            key));
    }
    if(hasExternalMetadata(descriptor)) {
        return foundation::Result<void>::failure(commandError(
            "Command.ExternalMetadataUnsupported",
            foundation::ErrorCategory::Validation,
            "Asynchronous read-only commands cannot declare external-effect metadata",
            key));
    }

    std::unique_lock lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::failure(commandError(
            "Command.RegistryFrozen",
            foundation::ErrorCategory::Conflict,
            "Command registration is closed",
            key));
    }
    const auto [unused, inserted] = entries_.emplace(
        key, Entry {std::move(descriptor), nullptr, std::move(handler)});
    static_cast<void>(unused);
    if(!inserted) {
        return foundation::Result<void>::failure(commandError(
            "Command.AlreadyRegistered",
            foundation::ErrorCategory::Conflict,
            "The exact command name and version are already registered",
            key));
    }
    return foundation::Result<void>::success();
}

foundation::Result<void> CommandRegistry::registerReadOnlyHandler(
    CommandDescriptor descriptor,
    std::shared_ptr<IReadOnlyCommandHandler> handler)
{
    const auto key = keyOf(descriptor);
    if(handler == nullptr) {
        return foundation::Result<void>::failure(commandError(
            "Command.InvalidHandler",
            foundation::ErrorCategory::Validation,
            "A read-only command handler is required",
            key));
    }
    if(descriptor.executionMode != ExecutionMode::Synchronous) {
        return foundation::Result<void>::failure(commandError(
            "Command.HandlerModeMismatch",
            foundation::ErrorCategory::Validation,
            "A synchronous read-only command requires an IReadOnlyCommandHandler",
            key));
    }
    if(!validExecutionScope(descriptor.scope)) {
        return foundation::Result<void>::failure(commandError(
            "Command.InvalidScope",
            foundation::ErrorCategory::Validation,
            "The command scope is invalid",
            key));
    }
    if(descriptor.sideEffect != SideEffectLevel::ReadOnly) {
        return foundation::Result<void>::failure(commandError(
            "Command.ReadOnlySideEffectMismatch",
            foundation::ErrorCategory::Validation,
            "A read-only command cannot declare side effects",
            key));
    }
    if(descriptor.undoable) {
        return foundation::Result<void>::failure(commandError(
            "Command.UndoUnsupported",
            foundation::ErrorCategory::Validation,
            "Read-only commands cannot create history entries",
            key));
    }
    if(descriptor.idempotent) {
        return foundation::Result<void>::failure(commandError(
            "Command.ReadOnlyIdempotencyUnsupported",
            foundation::ErrorCategory::Validation,
            "Synchronous read-only commands do not use the command idempotency store",
            key));
    }
    if(hasExternalMetadata(descriptor)) {
        return foundation::Result<void>::failure(commandError(
            "Command.ExternalMetadataUnsupported",
            foundation::ErrorCategory::Validation,
            "Synchronous read-only commands cannot declare external-effect metadata",
            key));
    }

    std::unique_lock lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::failure(commandError(
            "Command.RegistryFrozen",
            foundation::ErrorCategory::Conflict,
            "Command registration is closed",
            key));
    }
    const auto [unused, inserted] = entries_.emplace(
        key, Entry {std::move(descriptor), nullptr, nullptr, std::move(handler)});
    static_cast<void>(unused);
    if(!inserted) {
        return foundation::Result<void>::failure(commandError(
            "Command.AlreadyRegistered",
            foundation::ErrorCategory::Conflict,
            "The exact command name and version are already registered",
            key));
    }
    return foundation::Result<void>::success();
}

foundation::Result<void> CommandRegistry::registerExternalEffectHandler(
    CommandDescriptor descriptor,
    std::shared_ptr<IExternalEffectHandler> handler)
{
    const auto key = keyOf(descriptor);
    if(handler == nullptr) {
        return foundation::Result<void>::failure(commandError(
            "Command.InvalidHandler",
            foundation::ErrorCategory::Validation,
            "An external-effect command handler is required",
            key));
    }
    if(descriptor.executionMode != ExecutionMode::Synchronous) {
        return foundation::Result<void>::failure(commandError(
            "Command.HandlerModeMismatch",
            foundation::ErrorCategory::Validation,
            "An external-effect command must execute synchronously",
            key));
    }
    if(!validExecutionScope(descriptor.scope)) {
        return foundation::Result<void>::failure(commandError(
            "Command.InvalidScope",
            foundation::ErrorCategory::Validation,
            "The command scope is invalid",
            key));
    }
    if(!validExternalSideEffect(descriptor.sideEffect)) {
        return foundation::Result<void>::failure(commandError(
            "Command.ExternalSideEffectMismatch",
            foundation::ErrorCategory::Validation,
            "An external-effect handler requires an external side-effect level",
            key));
    }
    if(descriptor.undoable) {
        return foundation::Result<void>::failure(commandError(
            "Command.UndoUnsupported",
            foundation::ErrorCategory::Validation,
            "External side effects cannot create application undo entries",
            key));
    }
    if(!descriptor.idempotent) {
        return foundation::Result<void>::failure(commandError(
            "Command.ExternalIdempotencyRequired",
            foundation::ErrorCategory::Validation,
            "External side effects require a stable idempotency identity",
            key));
    }
    if(!validReplayPolicy(descriptor.replayPolicy)) {
        return foundation::Result<void>::failure(commandError(
            "Command.InvalidReplayPolicy",
            foundation::ErrorCategory::Validation,
            "The external-effect replay policy is invalid",
            key));
    }
    if(descriptor.effectGuards.empty()) {
        return foundation::Result<void>::failure(commandError(
            "Command.EffectGuardRequired",
            foundation::ErrorCategory::Validation,
            "External side effects require at least one declared effect guard",
            key));
    }
    if(descriptor.resources.empty()) {
        return foundation::Result<void>::failure(commandError(
            "Command.EffectResourceRequired",
            foundation::ErrorCategory::Validation,
            "External side effects require at least one declared resource claim",
            key));
    }
    for(const auto& claim : descriptor.resources) {
        if(claim.units == 0U) {
            return foundation::Result<void>::failure(commandError(
                "Command.InvalidEffectResourceUnits",
                foundation::ErrorCategory::Validation,
                "External-effect resource units must be greater than zero",
                key));
        }
    }

    std::unique_lock lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::failure(commandError(
            "Command.RegistryFrozen",
            foundation::ErrorCategory::Conflict,
            "Command registration is closed",
            key));
    }
    const auto [unused, inserted] = entries_.emplace(
        key,
        Entry {
            std::move(descriptor), nullptr, nullptr, nullptr, std::move(handler)});
    static_cast<void>(unused);
    if(!inserted) {
        return foundation::Result<void>::failure(commandError(
            "Command.AlreadyRegistered",
            foundation::ErrorCategory::Conflict,
            "The exact command name and version are already registered",
            key));
    }
    return foundation::Result<void>::success();
}

foundation::Result<CommandDescriptor> CommandRegistry::descriptor(
    const CommandKey& key,
    VersionResolution resolution) const
{
    auto entry = resolve(key, resolution);
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
    const CommandKey& key,
    VersionResolution resolution) const
{
    std::shared_lock lock(mutex_);
    const auto exact = entries_.find(key);
    if(resolution == VersionResolution::Exact && exact != entries_.end()) {
        return foundation::Result<Entry>::success(exact->second);
    }

    const auto first = entries_.lower_bound(CommandKey {key.name, foundation::Version {}});
    if(first == entries_.end() || first->first.name != key.name) {
        return foundation::Result<Entry>::failure(commandError(
            "Command.NotFound",
            foundation::ErrorCategory::NotFound,
            "The command is not registered",
            key));
    }
    if(resolution == VersionResolution::Compatible) {
        const Entry* compatible = nullptr;
        for(auto current = first;
            current != entries_.end() && current->first.name == key.name;
            ++current) {
            if(current->first.version.major == key.version.major
               && current->first.version >= key.version) {
                compatible = &current->second;
            }
        }
        if(compatible != nullptr) {
            return foundation::Result<Entry>::success(*compatible);
        }
    }
    return foundation::Result<Entry>::failure(commandError(
        "Command.UnsupportedVersion",
        foundation::ErrorCategory::Validation,
        "The requested command version is not supported",
        key));
}

void CommandRegistry::freeze()
{
    std::unique_lock lock(mutex_);
    frozen_ = true;
}

} // namespace lasercnc::runtime
