#include <lasercnc/infrastructure/toml_config_adapter.hpp>

#include <lasercnc/foundation/error.hpp>

#include "../../value_budget_support.hpp"

#include <toml.hpp>

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace lasercnc::infrastructure {
namespace {

using detail::ConversionBudget;
using detail::ValueBudgetExceeded;
using detail::budgetError;

foundation::Value fromToml(
    const toml::value& value,
    ConversionBudget& budget,
    std::size_t depth)
{
    budget.enter(depth);
    if(value.is_boolean()) return foundation::Value {value.as_boolean()};
    if(value.is_integer()) return foundation::Value {static_cast<std::int64_t>(value.as_integer())};
    if(value.is_floating()) return foundation::Value {value.as_floating()};
    if(value.is_string()) {
        auto text = value.as_string();
        budget.addText(text.size());
        return foundation::Value {std::move(text)};
    }
    if(value.is_array()) {
        foundation::Value::Array result;
        result.reserve(value.as_array().size());
        for(const auto& item : value.as_array()) {
            result.push_back(fromToml(item, budget, depth + 1U));
        }
        return foundation::Value {std::move(result)};
    }
    if(value.is_table()) {
        foundation::Value::Object result;
        for(const auto& [key, item] : value.as_table()) {
            budget.addText(key.size());
            result.emplace(key, fromToml(item, budget, depth + 1U));
        }
        return foundation::Value {std::move(result)};
    }
    throw std::invalid_argument("TOML date/time values are not part of Kernel Value");
}

toml::value toToml(const foundation::Value& value)
{
    switch(value.kind()) {
    case foundation::Value::Kind::Boolean: return toml::value {*value.getIf<bool>()};
    case foundation::Value::Kind::Integer: return toml::value {*value.getIf<std::int64_t>()};
    case foundation::Value::Kind::Number: return toml::value {*value.getIf<double>()};
    case foundation::Value::Kind::String: return toml::value {*value.getIf<std::string>()};
    case foundation::Value::Kind::Array: {
        toml::array result;
        for(const auto& item : *value.getIf<foundation::Value::Array>()) result.push_back(toToml(item));
        return toml::value {std::move(result)};
    }
    case foundation::Value::Kind::Object: {
        toml::table result;
        for(const auto& [key, item] : *value.getIf<foundation::Value::Object>()) {
            result.emplace(key, toToml(item));
        }
        return toml::value {std::move(result)};
    }
    case foundation::Value::Kind::Null:
        throw std::invalid_argument("TOML has no null value");
    }
    throw std::logic_error("Unknown Kernel Value kind");
}

foundation::Error configError(
    const char* code,
    const char* message,
    const std::exception& exception,
    std::string_view sourceName = {})
{
    foundation::Value::Object details {
        {"backend", foundation::Value {"toml11"}},
        {"reason", foundation::Value {exception.what()}},
    };
    if(!sourceName.empty()) details.emplace("source", foundation::Value {std::string(sourceName)});
    return foundation::makeError(
        code,
        foundation::ErrorCategory::Infrastructure,
        message,
        foundation::Value {std::move(details)});
}

} // namespace

foundation::Result<foundation::Value> TomlConfigAdapter::parse(
    std::string_view content,
    std::string_view sourceName) const
{
    if(content.size() > foundation::kernelValueBudget.maximumEncodedBytes) {
        return foundation::Result<foundation::Value>::failure(budgetError(
            "Config.InputBudgetExceeded",
            "TOML input exceeds the encoded byte budget",
            "encodedBytes",
            content.size(),
            foundation::kernelValueBudget.maximumEncodedBytes,
            "input"));
    }
    if(sourceName.size() > foundation::kernelValueBudget.maximumTextBytes) {
        return foundation::Result<foundation::Value>::failure(budgetError(
            "Config.SourceNameBudgetExceeded",
            "TOML source name exceeds the text byte budget",
            "textBytes",
            sourceName.size(),
            foundation::kernelValueBudget.maximumTextBytes,
            "sourceName"));
    }
    try {
        std::istringstream stream {std::string(content)};
        ConversionBudget budget;
        auto value = fromToml(
            toml::parse(stream, std::string(sourceName)),
            budget,
            1U);
        return foundation::Result<foundation::Value>::success(std::move(value));
    } catch(const ValueBudgetExceeded& exception) {
        return foundation::Result<foundation::Value>::failure(budgetError(
            "Config.ValueBudgetExceeded",
            "Parsed TOML exceeds the Kernel Value budget",
            exception,
            "input"));
    } catch(const std::exception& exception) {
        return foundation::Result<foundation::Value>::failure(
            configError("Config.ParseFailed", "TOML configuration parsing failed", exception, sourceName));
    }
}

foundation::Result<std::string> TomlConfigAdapter::serialize(const foundation::Value& root) const
{
    const auto assessment = foundation::assessValueBudget(root);
    if(!assessment.accepted()) {
        return foundation::Result<std::string>::failure(budgetError(
            "Config.ValueBudgetExceeded",
            "TOML value exceeds the Kernel Value budget",
            assessment,
            "value"));
    }
    try {
        if(root.kind() != foundation::Value::Kind::Object) {
            return foundation::Result<std::string>::failure(foundation::makeError(
                "Config.RootNotObject",
                foundation::ErrorCategory::Validation,
                "The TOML configuration root must be an object"));
        }
        auto output = toml::format(toToml(root));
        if(output.size() > foundation::kernelValueBudget.maximumEncodedBytes) {
            return foundation::Result<std::string>::failure(budgetError(
                "Config.OutputBudgetExceeded",
                "TOML output exceeds the encoded byte budget",
                "encodedBytes",
                output.size(),
                foundation::kernelValueBudget.maximumEncodedBytes,
                "output"));
        }
        return foundation::Result<std::string>::success(std::move(output));
    } catch(const std::exception& exception) {
        return foundation::Result<std::string>::failure(
            configError("Config.SerializeFailed", "TOML configuration serialization failed", exception));
    }
}

} // namespace lasercnc::infrastructure
