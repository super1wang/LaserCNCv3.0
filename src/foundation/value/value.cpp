#include <lasercnc/foundation/value.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace lasercnc::foundation {

Value::Value() noexcept
    : storage_(nullptr)
{
}

Value::Value(std::nullptr_t) noexcept
    : storage_(nullptr)
{
}

Value::Value(bool value) noexcept
    : storage_(value)
{
}

Value::Value(std::int64_t value) noexcept
    : storage_(value)
{
}

Value::Value(double value) noexcept
    : storage_(value)
{
}

Value::Value(std::string value)
    : storage_(std::move(value))
{
}

Value::Value(const char* value)
    : storage_(nullptr)
{
    if(value != nullptr) {
        storage_ = std::string(value);
    }
}

Value::Value(Array value)
    : storage_(std::move(value))
{
}

Value::Value(Object value)
    : storage_(std::move(value))
{
}

Value::Kind Value::kind() const noexcept
{
    return static_cast<Kind>(storage_.index());
}

const Value::Storage& Value::storage() const noexcept
{
    return storage_;
}

namespace {

std::size_t saturatedAdd(std::size_t left, std::size_t right) noexcept
{
    const auto maximum = std::numeric_limits<std::size_t>::max();
    return right > maximum - left ? maximum : left + right;
}

void assessValue(
    const Value& value,
    std::size_t depth,
    const ValueBudget& budget,
    ValueBudgetAssessment& assessment) noexcept
{
    if(!assessment.accepted()) return;
    assessment.maximumDepth = std::max(assessment.maximumDepth, depth);
    if(depth > budget.maximumDepth) {
        assessment.violation = ValueBudgetViolation::Depth;
        return;
    }
    assessment.nodes = saturatedAdd(assessment.nodes, 1U);
    if(assessment.nodes > budget.maximumNodes) {
        assessment.violation = ValueBudgetViolation::Nodes;
        return;
    }

    const auto addText = [&](std::size_t bytes) {
        assessment.textBytes = saturatedAdd(assessment.textBytes, bytes);
        if(assessment.textBytes > budget.maximumTextBytes) {
            assessment.violation = ValueBudgetViolation::TextBytes;
        }
    };
    if(const auto* text = value.getIf<std::string>()) {
        addText(text->size());
        return;
    }
    if(const auto* array = value.getIf<Value::Array>()) {
        for(const auto& item : *array) {
            assessValue(item, depth + 1U, budget, assessment);
            if(!assessment.accepted()) return;
        }
        return;
    }
    if(const auto* object = value.getIf<Value::Object>()) {
        for(const auto& [key, item] : *object) {
            addText(key.size());
            if(!assessment.accepted()) return;
            assessValue(item, depth + 1U, budget, assessment);
            if(!assessment.accepted()) return;
        }
    }
}

} // namespace

ValueBudgetAssessment assessValueBudget(
    const Value& value,
    const ValueBudget& budget) noexcept
{
    ValueBudgetAssessment assessment;
    if(budget.maximumDepth > kernelValueBudget.maximumDepth
       || budget.maximumNodes > kernelValueBudget.maximumNodes
       || budget.maximumTextBytes > kernelValueBudget.maximumTextBytes
       || budget.maximumEncodedBytes > kernelValueBudget.maximumEncodedBytes) {
        assessment.violation = ValueBudgetViolation::InvalidBudget;
        return assessment;
    }
    assessValue(value, 1U, budget, assessment);
    return assessment;
}

std::string_view valueBudgetViolationName(ValueBudgetViolation violation) noexcept
{
    switch(violation) {
    case ValueBudgetViolation::None: return "none";
    case ValueBudgetViolation::InvalidBudget: return "invalidBudget";
    case ValueBudgetViolation::Depth: return "depth";
    case ValueBudgetViolation::Nodes: return "nodes";
    case ValueBudgetViolation::TextBytes: return "textBytes";
    }
    return "unknown";
}

} // namespace lasercnc::foundation
