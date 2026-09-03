#include <lasercnc/infrastructure/toml_config_adapter.hpp>

#include <lasercnc/foundation/error.hpp>

#include <toml.hpp>

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace lasercnc::infrastructure {
namespace {

foundation::Value fromToml(const toml::value& value)
{
    if(value.is_boolean()) return foundation::Value {value.as_boolean()};
    if(value.is_integer()) return foundation::Value {static_cast<std::int64_t>(value.as_integer())};
    if(value.is_floating()) return foundation::Value {value.as_floating()};
    if(value.is_string()) return foundation::Value {value.as_string()};
    if(value.is_array()) {
        foundation::Value::Array result;
        result.reserve(value.as_array().size());
        for(const auto& item : value.as_array()) result.push_back(fromToml(item));
        return foundation::Value {std::move(result)};
    }
    if(value.is_table()) {
        foundation::Value::Object result;
        for(const auto& [key, item] : value.as_table()) result.emplace(key, fromToml(item));
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
    try {
        std::istringstream stream {std::string(content)};
        return foundation::Result<foundation::Value>::success(
            fromToml(toml::parse(stream, std::string(sourceName))));
    } catch(const std::exception& exception) {
        return foundation::Result<foundation::Value>::failure(
            configError("Config.ParseFailed", "TOML configuration parsing failed", exception, sourceName));
    }
}

foundation::Result<std::string> TomlConfigAdapter::serialize(const foundation::Value& root) const
{
    try {
        if(root.kind() != foundation::Value::Kind::Object) {
            return foundation::Result<std::string>::failure(foundation::makeError(
                "Config.RootNotObject",
                foundation::ErrorCategory::Validation,
                "The TOML configuration root must be an object"));
        }
        return foundation::Result<std::string>::success(toml::format(toToml(root)));
    } catch(const std::exception& exception) {
        return foundation::Result<std::string>::failure(
            configError("Config.SerializeFailed", "TOML configuration serialization failed", exception));
    }
}

} // namespace lasercnc::infrastructure
