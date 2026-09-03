#include <lasercnc/state/object_type_registry.hpp>

#include <lasercnc/foundation/error.hpp>

#include <algorithm>
#include <exception>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace lasercnc::state {
namespace {

foundation::Error typeError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const kernel::ObjectTypeId& type,
    std::shared_ptr<const foundation::Error> cause = nullptr)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {foundation::Value::Object {
            {"objectType", foundation::Value {std::string(type.value())}}}},
        foundation::Severity::Error,
        std::move(cause));
}

const ObjectTypeVersion* findVersion(
    const ObjectTypeDefinition& definition,
    foundation::Version version)
{
    const auto found = std::ranges::find(definition.versions, version, &ObjectTypeVersion::version);
    return found == definition.versions.end() ? nullptr : &*found;
}

foundation::Result<void> validateDefinition(const ObjectTypeDefinition& definition)
{
    const auto& type = definition.descriptor.type;
    const auto policy = definition.descriptor.persistencePolicy;
    if(policy != ObjectPersistencePolicy::Durable
       && policy != ObjectPersistencePolicy::Transient) {
        return foundation::Result<void>::failure(typeError(
            "ObjectType.InvalidPersistencePolicy", foundation::ErrorCategory::Validation,
            "The object persistence policy is not supported", type));
    }
    std::set<foundation::Version> versions;
    for(const auto& contract : definition.versions) {
        if(contract.validator == nullptr || contract.references == nullptr
           || contract.version > definition.descriptor.currentVersion
           || !versions.insert(contract.version).second) {
            return foundation::Result<void>::failure(typeError(
                "ObjectType.InvalidVersionContract", foundation::ErrorCategory::Validation,
                "Object versions require unique supported identities and complete callbacks", type));
        }
    }
    if(!versions.contains(definition.descriptor.currentVersion)) {
        return foundation::Result<void>::failure(typeError(
            "ObjectType.CurrentVersionMissing", foundation::ErrorCategory::Validation,
            "The current object schema version must be registered", type));
    }
    std::map<foundation::Version, foundation::Version> edges;
    for(const auto& migration : definition.migrations) {
        if(migration.migration == nullptr || migration.from >= migration.to
           || !versions.contains(migration.from) || !versions.contains(migration.to)
           || !edges.emplace(migration.from, migration.to).second) {
            return foundation::Result<void>::failure(typeError(
                "ObjectType.InvalidMigrationChain", foundation::ErrorCategory::Validation,
                "Migration edges must be unique explicit forward steps between registered versions", type));
        }
    }
    for(const auto version : versions) {
        auto cursor = version;
        while(cursor != definition.descriptor.currentVersion) {
            const auto next = edges.find(cursor);
            if(next == edges.end()) {
                return foundation::Result<void>::failure(typeError(
                    "ObjectType.MigrationPathMissing", foundation::ErrorCategory::Validation,
                    "Every supported old version must have an explicit path to the current version", type));
            }
            cursor = next->second;
        }
    }
    return foundation::Result<void>::success();
}

foundation::Result<void> validateData(
    const kernel::ObjectTypeId& type,
    const ObjectTypeVersion& contract,
    const foundation::Value& data)
{
    try {
        auto result = contract.validator->validate(data);
        if(!result) {
            return foundation::Result<void>::failure(typeError(
                "ObjectType.ValidationFailed", foundation::ErrorCategory::Validation,
                "Object data does not satisfy its exact schema version", type,
                std::make_shared<const foundation::Error>(std::move(result).error())));
        }
        return result;
    } catch(const std::exception& exception) {
        return foundation::Result<void>::failure(typeError(
            "ObjectType.ValidatorException", foundation::ErrorCategory::Internal,
            "The object validator raised an exception", type,
            std::make_shared<const foundation::Error>(foundation::makeError(
                "ObjectType.CallbackException", foundation::ErrorCategory::Internal,
                exception.what()))));
    } catch(...) {
        return foundation::Result<void>::failure(typeError(
            "ObjectType.ValidatorException", foundation::ErrorCategory::Internal,
            "The object validator raised an unknown exception", type));
    }
}

} // namespace

foundation::Result<void> ObjectTypeRegistry::registerType(ObjectTypeDefinition definition)
{
    auto valid = validateDefinition(definition);
    if(!valid) {
        return valid;
    }
    std::ranges::sort(definition.versions, {}, &ObjectTypeVersion::version);
    std::ranges::sort(definition.migrations, {}, &ObjectTypeMigration::from);
    std::unique_lock lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::failure(typeError(
            "ObjectType.RegistryFrozen", foundation::ErrorCategory::Conflict,
            "Object type registration is closed", definition.descriptor.type));
    }
    const auto type = definition.descriptor.type;
    if(!definitions_.emplace(type, std::move(definition)).second) {
        return foundation::Result<void>::failure(typeError(
            "ObjectType.AlreadyRegistered", foundation::ErrorCategory::Conflict,
            "The object type already has a registered definition", type));
    }
    return foundation::Result<void>::success();
}

void ObjectTypeRegistry::freeze()
{
    std::unique_lock lock(mutex_);
    frozen_ = true;
}

void ObjectTypeRegistry::remove(const kernel::ObjectTypeId& type)
{
    std::unique_lock lock(mutex_);
    definitions_.erase(type);
}

bool ObjectTypeRegistry::frozen() const
{
    std::shared_lock lock(mutex_);
    return frozen_;
}

std::size_t ObjectTypeRegistry::size() const
{
    std::shared_lock lock(mutex_);
    return definitions_.size();
}

foundation::Result<ObjectTypeDefinition> ObjectTypeRegistry::resolve(
    const kernel::ObjectTypeId& type) const
{
    std::shared_lock lock(mutex_);
    const auto found = definitions_.find(type);
    if(found == definitions_.end()) {
        return foundation::Result<ObjectTypeDefinition>::failure(typeError(
            "ObjectType.NotFound", foundation::ErrorCategory::NotFound,
            "The object type is not registered", type));
    }
    return foundation::Result<ObjectTypeDefinition>::success(found->second);
}

foundation::Result<ObjectTypeDescriptor> ObjectTypeRegistry::descriptor(
    const kernel::ObjectTypeId& type) const
{
    auto definition = resolve(type);
    if(!definition) {
        return foundation::Result<ObjectTypeDescriptor>::failure(std::move(definition).error());
    }
    return foundation::Result<ObjectTypeDescriptor>::success(definition.value().descriptor);
}

std::vector<ObjectTypeDescriptor> ObjectTypeRegistry::descriptors() const
{
    std::shared_lock lock(mutex_);
    std::vector<ObjectTypeDescriptor> descriptors;
    descriptors.reserve(definitions_.size());
    for(const auto& [unused, definition] : definitions_) {
        static_cast<void>(unused);
        descriptors.push_back(definition.descriptor);
    }
    return descriptors;
}

foundation::Result<std::vector<foundation::Version>> ObjectTypeRegistry::versions(
    const kernel::ObjectTypeId& type) const
{
    auto definition = resolve(type);
    if(!definition) {
        return foundation::Result<std::vector<foundation::Version>>::failure(
            std::move(definition).error());
    }
    std::vector<foundation::Version> result;
    for(const auto& contract : definition.value().versions) {
        result.push_back(contract.version);
    }
    return foundation::Result<std::vector<foundation::Version>>::success(std::move(result));
}

foundation::Result<void> ObjectTypeRegistry::validate(
    const kernel::ObjectTypeId& type,
    foundation::Version version,
    const foundation::Value& data) const
{
    auto definition = resolve(type);
    if(!definition) {
        return foundation::Result<void>::failure(std::move(definition).error());
    }
    const auto* contract = findVersion(definition.value(), version);
    if(contract == nullptr) {
        return foundation::Result<void>::failure(typeError(
            "ObjectType.UnsupportedVersion", foundation::ErrorCategory::Conflict,
            "The exact object schema version is not registered", type));
    }
    return validateData(type, *contract, data);
}

foundation::Result<foundation::Value> ObjectTypeRegistry::migrate(
    const kernel::ObjectTypeId& type,
    foundation::Version from,
    foundation::Version to,
    const foundation::Value& data) const
{
    auto resolved = resolve(type);
    if(!resolved) {
        return foundation::Result<foundation::Value>::failure(std::move(resolved).error());
    }
    const auto& definition = resolved.value();
    const auto* source = findVersion(definition, from);
    if(source == nullptr || findVersion(definition, to) == nullptr || from > to) {
        return foundation::Result<foundation::Value>::failure(typeError(
            "ObjectType.MigrationVersionUnsupported", foundation::ErrorCategory::Conflict,
            "Migration requires supported versions and cannot downgrade object data", type));
    }
    std::vector<const ObjectTypeMigration*> path;
    auto cursor = from;
    while(cursor != to) {
        const auto step = std::ranges::find(definition.migrations, cursor, &ObjectTypeMigration::from);
        if(step == definition.migrations.end() || step->to > to) {
            return foundation::Result<foundation::Value>::failure(typeError(
                "ObjectType.MigrationPathMissing", foundation::ErrorCategory::Conflict,
                "No explicit migration path reaches the requested target version", type));
        }
        path.push_back(&*step);
        cursor = step->to;
    }
    auto valid = validateData(type, *source, data);
    if(!valid) {
        return foundation::Result<foundation::Value>::failure(std::move(valid).error());
    }
    auto migrated = data;
    for(const auto* step : path) {
        try {
            auto result = step->migration->migrate(migrated);
            if(!result) {
                return foundation::Result<foundation::Value>::failure(typeError(
                    "ObjectType.MigrationFailed", foundation::ErrorCategory::Validation,
                    "An object migration step failed", type,
                    std::make_shared<const foundation::Error>(std::move(result).error())));
            }
            migrated = std::move(result).value();
        } catch(...) {
            return foundation::Result<foundation::Value>::failure(typeError(
                "ObjectType.MigrationException", foundation::ErrorCategory::Internal,
                "An object migration step raised an exception", type));
        }
        valid = validateData(type, *findVersion(definition, step->to), migrated);
        if(!valid) {
            return foundation::Result<foundation::Value>::failure(std::move(valid).error());
        }
    }
    return foundation::Result<foundation::Value>::success(std::move(migrated));
}

foundation::Result<std::vector<kernel::ObjectId>> ObjectTypeRegistry::references(
    const kernel::ObjectTypeId& type,
    foundation::Version version,
    const foundation::Value& data) const
{
    auto resolved = resolve(type);
    if(!resolved) {
        return foundation::Result<std::vector<kernel::ObjectId>>::failure(std::move(resolved).error());
    }
    const auto* contract = findVersion(resolved.value(), version);
    if(contract == nullptr) {
        return foundation::Result<std::vector<kernel::ObjectId>>::failure(typeError(
            "ObjectType.UnsupportedVersion", foundation::ErrorCategory::Conflict,
            "The exact object schema version is not registered", type));
    }
    auto valid = validateData(type, *contract, data);
    if(!valid) {
        return foundation::Result<std::vector<kernel::ObjectId>>::failure(std::move(valid).error());
    }
    try {
        auto result = contract->references->enumerate(data);
        if(!result) {
            return foundation::Result<std::vector<kernel::ObjectId>>::failure(typeError(
                "ObjectType.ReferenceEnumerationFailed", foundation::ErrorCategory::Validation,
                "Object reference enumeration failed", type,
                std::make_shared<const foundation::Error>(std::move(result).error())));
        }
        auto references = std::move(result).value();
        std::ranges::sort(references);
        references.erase(std::unique(references.begin(), references.end()), references.end());
        return foundation::Result<std::vector<kernel::ObjectId>>::success(std::move(references));
    } catch(...) {
        return foundation::Result<std::vector<kernel::ObjectId>>::failure(typeError(
            "ObjectType.ReferenceEnumerationException", foundation::ErrorCategory::Internal,
            "Object reference enumeration raised an exception", type));
    }
}

} // namespace lasercnc::state
