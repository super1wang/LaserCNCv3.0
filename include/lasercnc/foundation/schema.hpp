#pragma once

#include <lasercnc/foundation/strong_id.hpp>
#include <lasercnc/foundation/value.hpp>
#include <lasercnc/foundation/version.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace lasercnc::foundation {

struct SchemaIdTag;
using SchemaId = StrongId<SchemaIdTag>;

enum class SchemaKind : std::uint8_t {
    Any,
    Null,
    Boolean,
    Integer,
    Number,
    String,
    Array,
    Object
};

class Schema final {
public:
    [[nodiscard]] static Result<Schema> create(
        SchemaId id,
        Version version,
        SchemaKind rootKind,
        Value constraints = Value {Value::Object {}},
        std::optional<std::string> unit = std::nullopt);

    [[nodiscard]] const SchemaId& id() const noexcept;
    [[nodiscard]] const Version& version() const noexcept;
    [[nodiscard]] SchemaKind rootKind() const noexcept;
    [[nodiscard]] const Value& constraints() const noexcept;
    [[nodiscard]] const std::optional<std::string>& unit() const noexcept;

private:
    Schema(
        SchemaId id,
        Version version,
        SchemaKind rootKind,
        Value constraints,
        std::optional<std::string> unit);

    SchemaId id_;
    Version version_;
    SchemaKind rootKind_;
    Value constraints_;
    std::optional<std::string> unit_;
};

class ISchemaValidator {
public:
    virtual ~ISchemaValidator() = default;

    [[nodiscard]] virtual Result<void> validate(
        const Schema& schema,
        const Value& value) const = 0;
};

} // namespace lasercnc::foundation
