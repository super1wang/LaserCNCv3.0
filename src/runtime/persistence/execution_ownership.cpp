#include <lasercnc/persistence/persistence_service.hpp>
#include <lasercnc/runtime/command.hpp>

#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace lasercnc::persistence {
namespace {
using foundation::Result;
using foundation::Value;

foundation::Error invalidOwnership()
{
    return foundation::makeError("Persistence.ExecutionOwnershipInvalid", foundation::ErrorCategory::Infrastructure,
        "A durable execution request has an unsupported format or invalid ownership metadata");
}

const Value* field(const Value::Object& root, std::string_view name)
{
    const auto found = root.find(name);
    return found == root.end() ? nullptr : &found->second;
}

template<typename T>
const T* fieldAs(const Value::Object& root, std::string_view name)
{
    const auto* value = field(root, name);
    return value == nullptr ? nullptr : value->getIf<T>();
}

bool formatMatches(const Value::Object& root, std::string_view expected)
{
    const auto* format = fieldAs<std::string>(root, "format");
    const auto* version = fieldAs<std::int64_t>(root, "version");
    return format != nullptr && *format == expected && version != nullptr && *version == 1;
}

template<typename Id>
Result<std::optional<Id>> optionalIdentity(const Value::Object& root, std::string_view name)
{
    const auto* value = field(root, name);
    if(value == nullptr) { return Result<std::optional<Id>>::failure(invalidOwnership()); }
    if(value->kind() == Value::Kind::Null) { return Result<std::optional<Id>>::success(std::nullopt); }
    const auto* text = value->getIf<std::string>();
    if(text == nullptr) { return Result<std::optional<Id>>::failure(invalidOwnership()); }
    auto id = Id::create(*text);
    if(!id) { return Result<std::optional<Id>>::failure(invalidOwnership()); }
    return Result<std::optional<Id>>::success(std::move(id).value());
}

template<typename Id>
bool requiredIdentity(const Value::Object& root, std::string_view name)
{
    auto value = optionalIdentity<Id>(root, name);
    return value && value.value().has_value();
}

bool validVersion(const Value::Object& root, std::string_view name)
{
    const auto* version = fieldAs<Value::Object>(root, name);
    if(version == nullptr) { return false; }
    for(const auto* component : {"major", "minor", "patch"}) {
        const auto* number = fieldAs<std::int64_t>(*version, component);
        if(number == nullptr || *number < 0 || *number > std::numeric_limits<std::uint32_t>::max()) { return false; }
    }
    return true;
}

bool validEffectSignature(const Value::Object& root, runtime::ReplayPolicy policy)
{
    const auto* persistedPolicy = fieldAs<std::string>(root, "replayPolicy");
    const auto* effect = fieldAs<std::int64_t>(root, "sideEffect");
    const auto* resolution = fieldAs<std::int64_t>(root, "versionResolution");
    if(!formatMatches(root, "lasercnc.external-effect-signature")
       || !requiredIdentity<kernel::CommandName>(root, "command")
       || !requiredIdentity<kernel::SessionId>(root, "sessionId")
       || !validVersion(root, "requestedVersion") || !validVersion(root, "resolvedVersion")
       || persistedPolicy == nullptr || *persistedPolicy != runtime::replayPolicyName(policy)
       || effect == nullptr || *effect < static_cast<std::int64_t>(runtime::SideEffectLevel::FileSystemWrite)
       || *effect > static_cast<std::int64_t>(runtime::SideEffectLevel::LaserControl)
       || resolution == nullptr || (*resolution != 0 && *resolution != 1)
       || fieldAs<Value::Array>(root, "resources") == nullptr
       || fieldAs<Value::Array>(root, "effectGuards") == nullptr || field(root, "arguments") == nullptr) {
        return false;
    }
    return true;
}
} // namespace

foundation::Result<std::vector<PersistenceService::ExecutionOwnership>> PersistenceService::executionOwnerships() const
{
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return Result<std::vector<ExecutionOwnership>>::failure(foundation::makeError(
            "Persistence.NotReady", foundation::ErrorCategory::Conflict, "Persistence is not initialized"));
    }
    try { return executionOwnershipsUnlocked(); }
    catch(...) {
        return Result<std::vector<ExecutionOwnership>>::failure(foundation::makeError(
            "Persistence.ExecutionOwnershipReadFailed", foundation::ErrorCategory::Internal,
            "Durable execution ownership authentication raised an exception"));
    }
}

foundation::Result<std::vector<PersistenceService::ExecutionOwnership>> PersistenceService::executionOwnershipsUnlocked() const
{
    std::vector<ExecutionOwnership> result;
    const auto append = [&](const Value& payload) -> Result<void> {
        const auto* root = payload.getIf<Value::Object>();
        if(root == nullptr) { return Result<void>::failure(invalidOwnership()); }
        auto project = optionalIdentity<kernel::ProjectId>(*root, "projectId");
        auto document = optionalIdentity<kernel::DocumentId>(*root, "documentId");
        if(!project || !document || (document.value().has_value() && !project.value().has_value())) {
            return Result<void>::failure(invalidOwnership());
        }
        if(project.value()) { result.push_back({*project.value(), document.value()}); }
        return Result<void>::success();
    };
    // Reuse authenticated history readers, but never interpret the records as executable work.
    // 中文翻译：复用已认证历史读取链，但绝不把这些记录解释成可执行工作。
    auto tasks = backend_->query("SELECT task_id FROM task_history ORDER BY task_id");
    if(!tasks) { return Result<std::vector<ExecutionOwnership>>::failure(std::move(tasks).error()); }
    for(const auto& row : tasks.value()) {
        const auto* text = fieldAs<std::string>(row, "task_id");
        auto identity = text != nullptr ? kernel::TaskId::create(*text) : Result<kernel::TaskId>::failure(invalidOwnership());
        if(!identity) { return Result<std::vector<ExecutionOwnership>>::failure(std::move(identity).error()); }
        Value payload;
        auto history = taskHistoryUnlocked(identity.value(), &payload);
        if(!history) { return Result<std::vector<ExecutionOwnership>>::failure(std::move(history).error()); }
        const auto* root = payload.getIf<Value::Object>();
        if(!history.value() || root == nullptr || !formatMatches(*root, "lasercnc.task-acceptance")) {
            return Result<std::vector<ExecutionOwnership>>::failure(invalidOwnership());
        }
        auto appended = append(payload);
        if(!appended) { return Result<std::vector<ExecutionOwnership>>::failure(std::move(appended).error()); }
    }
    auto effects = backend_->query("SELECT idempotency_key FROM external_effects ORDER BY idempotency_key");
    if(!effects) { return Result<std::vector<ExecutionOwnership>>::failure(std::move(effects).error()); }
    for(const auto& row : effects.value()) {
        const auto* text = fieldAs<std::string>(row, "idempotency_key");
        auto identity = text != nullptr ? kernel::IdempotencyKey::create(*text)
            : Result<kernel::IdempotencyKey>::failure(invalidOwnership());
        if(!identity) { return Result<std::vector<ExecutionOwnership>>::failure(std::move(identity).error()); }
        Value payload;
        auto history = externalEffectUnlocked(identity.value(), &payload);
        if(!history) { return Result<std::vector<ExecutionOwnership>>::failure(std::move(history).error()); }
        const auto* root = payload.getIf<Value::Object>();
        if(!history.value() || root == nullptr || !validEffectSignature(*root, history.value()->replayPolicy)) {
            return Result<std::vector<ExecutionOwnership>>::failure(invalidOwnership());
        }
        auto appended = append(payload);
        if(!appended) { return Result<std::vector<ExecutionOwnership>>::failure(std::move(appended).error()); }
    }
    return Result<std::vector<ExecutionOwnership>>::success(std::move(result));
}
} // namespace lasercnc::persistence
