#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/value.hpp>
#include <lasercnc/foundation/version.hpp>
#include <lasercnc/kernel/identifiers.hpp>

#include <cstdint>
#include <cstddef>
#include <map>
#include <memory>
#include <shared_mutex>
#include <vector>

namespace lasercnc::state {

enum class ObjectPersistencePolicy : std::uint8_t {
    Durable,
    Transient
};

struct ObjectTypeDescriptor final {
    kernel::ObjectTypeId type;
    foundation::Version currentVersion;
    ObjectPersistencePolicy persistencePolicy{ObjectPersistencePolicy::Durable};
};

class IObjectTypeValidator {
public:
    virtual ~IObjectTypeValidator() = default;
    [[nodiscard]] virtual foundation::Result<void> validate(
        const foundation::Value& data) const = 0;
};

class IObjectReferenceEnumerator {
public:
    virtual ~IObjectReferenceEnumerator() = default;
    [[nodiscard]] virtual foundation::Result<std::vector<kernel::ObjectId>> enumerate(
        const foundation::Value& data) const = 0;
};

class IObjectMigration {
public:
    virtual ~IObjectMigration() = default;
    [[nodiscard]] virtual foundation::Result<foundation::Value> migrate(
        const foundation::Value& data) const = 0;
};

struct ObjectTypeVersion final {
    foundation::Version version;
    std::shared_ptr<const IObjectTypeValidator> validator;
    std::shared_ptr<const IObjectReferenceEnumerator> references;
};

struct ObjectTypeMigration final {
    foundation::Version from;
    foundation::Version to;
    std::shared_ptr<const IObjectMigration> migration;
};

struct ObjectTypeDefinition final {
    ObjectTypeDescriptor descriptor;
    std::vector<ObjectTypeVersion> versions;
    std::vector<ObjectTypeMigration> migrations;
};

class ObjectTypeRegistry final {
public:
    [[nodiscard]] foundation::Result<void> registerType(ObjectTypeDefinition definition);
    void freeze();
    [[nodiscard]] bool frozen() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] foundation::Result<ObjectTypeDescriptor> descriptor(
        const kernel::ObjectTypeId& type) const;
    [[nodiscard]] std::vector<ObjectTypeDescriptor> descriptors() const;
    [[nodiscard]] foundation::Result<std::vector<foundation::Version>> versions(
        const kernel::ObjectTypeId& type) const;

    [[nodiscard]] foundation::Result<void> validate(
        const kernel::ObjectTypeId& type,
        foundation::Version version,
        const foundation::Value& data) const;
    [[nodiscard]] foundation::Result<foundation::Value> migrate(
        const kernel::ObjectTypeId& type,
        foundation::Version from,
        foundation::Version to,
        const foundation::Value& data) const;
    [[nodiscard]] foundation::Result<std::vector<kernel::ObjectId>> references(
        const kernel::ObjectTypeId& type,
        foundation::Version version,
        const foundation::Value& data) const;

private:
    [[nodiscard]] foundation::Result<ObjectTypeDefinition> resolve(
        const kernel::ObjectTypeId& type) const;

    mutable std::shared_mutex mutex_;
    std::map<kernel::ObjectTypeId, ObjectTypeDefinition> definitions_;
    bool frozen_{false};
};

} // namespace lasercnc::state
