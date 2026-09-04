#include <lasercnc/foundation/schema.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace lasercnc::foundation {
namespace {

std::size_t budgetLimit(ValueBudgetViolation violation) noexcept
{
    switch(violation) {
    case ValueBudgetViolation::Depth: return kernelValueBudget.maximumDepth;
    case ValueBudgetViolation::Nodes: return kernelValueBudget.maximumNodes;
    case ValueBudgetViolation::TextBytes: return kernelValueBudget.maximumTextBytes;
    case ValueBudgetViolation::None:
    case ValueBudgetViolation::InvalidBudget:
        return 0U;
    }
    return 0U;
}

std::size_t budgetActual(const ValueBudgetAssessment& assessment) noexcept
{
    switch(assessment.violation) {
    case ValueBudgetViolation::Depth: return assessment.maximumDepth;
    case ValueBudgetViolation::Nodes: return assessment.nodes;
    case ValueBudgetViolation::TextBytes: return assessment.textBytes;
    case ValueBudgetViolation::None:
    case ValueBudgetViolation::InvalidBudget:
        return 0U;
    }
    return 0U;
}

} // namespace

Result<Schema> Schema::create(
    SchemaId id,
    Version version,
    SchemaKind rootKind,
    Value constraints,
    std::optional<std::string> unit)
{
    switch(rootKind) {
    case SchemaKind::Any:
    case SchemaKind::Null:
    case SchemaKind::Boolean:
    case SchemaKind::Integer:
    case SchemaKind::Number:
    case SchemaKind::String:
    case SchemaKind::Array:
    case SchemaKind::Object:
        break;
    default:
        return Result<Schema>::failure(makeError(
            "Foundation.SchemaKindInvalid",
            ErrorCategory::Validation,
            "Schema root kind must be a declared SchemaKind"));
    }

    if(constraints.kind() != Value::Kind::Object) {
        return Result<Schema>::failure(makeError(
            "Foundation.SchemaConstraintsInvalid",
            ErrorCategory::Validation,
            "Schema constraints must be a Value object"));
    }

    const auto constraintBudget = assessValueBudget(constraints);
    if(!constraintBudget.accepted()) {
        return Result<Schema>::failure(makeError(
            "Foundation.SchemaBudgetExceeded",
            ErrorCategory::Validation,
            "Schema constraints exceed the Kernel Value budget",
            Value {Value::Object {
                {"actual", Value {std::to_string(budgetActual(constraintBudget))}},
                {"dimension", Value {std::string(valueBudgetViolationName(
                    constraintBudget.violation))}},
                {"limit", Value {std::to_string(budgetLimit(constraintBudget.violation))}},
                {"material", Value {"constraints"}},
            }}));
    }

    if(unit.has_value()) {
        if(unit->size() > kernelValueBudget.maximumTextBytes) {
            return Result<Schema>::failure(makeError(
                "Foundation.SchemaUnitBudgetExceeded",
                ErrorCategory::Validation,
                "Schema unit exceeds the Kernel text byte budget",
                Value {Value::Object {
                    {"actual", Value {std::to_string(unit->size())}},
                    {"dimension", Value {"textBytes"}},
                    {"limit", Value {std::to_string(kernelValueBudget.maximumTextBytes)}},
                    {"material", Value {"unit"}},
                }}));
        }
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
