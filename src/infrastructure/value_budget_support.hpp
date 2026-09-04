#pragma once

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/foundation/value.hpp>

#include <cstddef>
#include <exception>
#include <string>
#include <string_view>

namespace lasercnc::infrastructure::detail {

class ValueBudgetExceeded final : public std::exception {
public:
    ValueBudgetExceeded(
        foundation::ValueBudgetViolation violation,
        std::size_t actual,
        std::size_t limit) noexcept
        : violation_(violation), actual_(actual), limit_(limit)
    {
    }

    [[nodiscard]] foundation::ValueBudgetViolation violation() const noexcept { return violation_; }
    [[nodiscard]] std::size_t actual() const noexcept { return actual_; }
    [[nodiscard]] std::size_t limit() const noexcept { return limit_; }
    [[nodiscard]] const char* what() const noexcept override { return "Kernel Value budget exceeded"; }

private:
    foundation::ValueBudgetViolation violation_;
    std::size_t actual_;
    std::size_t limit_;
};

class ConversionBudget final {
public:
    void enter(std::size_t depth)
    {
        if(depth > foundation::kernelValueBudget.maximumDepth) {
            throw ValueBudgetExceeded(
                foundation::ValueBudgetViolation::Depth,
                depth,
                foundation::kernelValueBudget.maximumDepth);
        }
        if(nodes_ == foundation::kernelValueBudget.maximumNodes) {
            throw ValueBudgetExceeded(
                foundation::ValueBudgetViolation::Nodes,
                nodes_ + 1U,
                foundation::kernelValueBudget.maximumNodes);
        }
        ++nodes_;
    }

    void addText(std::size_t bytes)
    {
        const auto maximum = foundation::kernelValueBudget.maximumTextBytes;
        if(bytes > maximum - textBytes_) {
            throw ValueBudgetExceeded(
                foundation::ValueBudgetViolation::TextBytes,
                maximum + 1U,
                maximum);
        }
        textBytes_ += bytes;
    }

private:
    std::size_t nodes_{0U};
    std::size_t textBytes_{0U};
};

inline std::size_t budgetLimit(foundation::ValueBudgetViolation violation) noexcept
{
    switch(violation) {
    case foundation::ValueBudgetViolation::Depth:
        return foundation::kernelValueBudget.maximumDepth;
    case foundation::ValueBudgetViolation::Nodes:
        return foundation::kernelValueBudget.maximumNodes;
    case foundation::ValueBudgetViolation::TextBytes:
        return foundation::kernelValueBudget.maximumTextBytes;
    case foundation::ValueBudgetViolation::None:
    case foundation::ValueBudgetViolation::InvalidBudget:
        return 0U;
    }
    return 0U;
}

inline std::size_t budgetActual(const foundation::ValueBudgetAssessment& assessment) noexcept
{
    switch(assessment.violation) {
    case foundation::ValueBudgetViolation::Depth: return assessment.maximumDepth;
    case foundation::ValueBudgetViolation::Nodes: return assessment.nodes;
    case foundation::ValueBudgetViolation::TextBytes: return assessment.textBytes;
    case foundation::ValueBudgetViolation::None:
    case foundation::ValueBudgetViolation::InvalidBudget:
        return 0U;
    }
    return 0U;
}

inline foundation::Error budgetError(
    const char* code,
    const char* message,
    std::string_view dimension,
    std::size_t actual,
    std::size_t limit,
    std::string_view material)
{
    return foundation::makeError(
        code,
        foundation::ErrorCategory::Validation,
        message,
        foundation::Value {foundation::Value::Object {
            {"actual", foundation::Value {std::to_string(actual)}},
            {"dimension", foundation::Value {std::string(dimension)}},
            {"limit", foundation::Value {std::to_string(limit)}},
            {"material", foundation::Value {std::string(material)}},
        }});
}

inline foundation::Error budgetError(
    const char* code,
    const char* message,
    const foundation::ValueBudgetAssessment& assessment,
    std::string_view material)
{
    return budgetError(
        code,
        message,
        foundation::valueBudgetViolationName(assessment.violation),
        budgetActual(assessment),
        budgetLimit(assessment.violation),
        material);
}

inline foundation::Error budgetError(
    const char* code,
    const char* message,
    const ValueBudgetExceeded& exception,
    std::string_view material)
{
    return budgetError(
        code,
        message,
        foundation::valueBudgetViolationName(exception.violation()),
        exception.actual(),
        exception.limit(),
        material);
}

} // namespace lasercnc::infrastructure::detail
