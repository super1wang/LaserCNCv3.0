#include <lasercnc/foundation/schema.hpp>

#include <algorithm>
#include <cctype>
#include <utility>

namespace lasercnc::foundation {

Result<Schema> Schema::create(
    SchemaId id,
    Version version,
    SchemaKind rootKind,
    Value constraints,
    std::optional<std::string> unit)
{
    if(constraints.kind() != Value::Kind::Object) {
        return Result<Schema>::failure(makeError(
            "Foundation.SchemaConstraintsInvalid",
            ErrorCategory::Validation,
            "Schema constraints must be a Value object"));
    }

    if(unit.has_value()) {
        const bool onlyWhitespace = unit->empty()
            || std::all_of(
                unit->begin(),
                unit->end(),
                [](unsigned char character) { return std::isspace(character) != 0; });
        if(onlyWhitespace) {
            return Result<Schema>::failure(makeError(
                "Foundation.SchemaUnitInvalid",
                ErrorCategory::Validation,
                "Schema unit must not be empty"));
        }
    }

    return Result<Schema>::success(
        Schema(std::move(id), version, rootKind, std::move(constraints), std::move(unit)));
}

Schema::Schema(
    SchemaId id,
    Version version,
    SchemaKind rootKind,
    Value constraints,
    std::optional<std::string> unit)
    : id_(std::move(id))
    , version_(version)
    , rootKind_(rootKind)
    , constraints_(std::move(constraints))
    , unit_(std::move(unit))
{
}

const SchemaId& Schema::id() const noexcept
{
    return id_;
}

const Version& Schema::version() const noexcept
{
    return version_;
}

SchemaKind Schema::rootKind() const noexcept
{
    return rootKind_;
}

const Value& Schema::constraints() const noexcept
{
    return constraints_;
}

const std::optional<std::string>& Schema::unit() const noexcept
{
    return unit_;
}

} // namespace lasercnc::foundation
