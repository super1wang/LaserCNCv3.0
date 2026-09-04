#include <lasercnc/infrastructure/jsoncons_adapter.hpp>

#include <lasercnc/foundation/error.hpp>

#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace lasercnc::infrastructure {
namespace {

using Json = jsoncons::json;

Json toJson(const foundation::Value& value)
{
    switch(value.kind()) {
    case foundation::Value::Kind::Null:
        return Json::null();
    case foundation::Value::Kind::Boolean:
        return Json {*value.getIf<bool>()};
    case foundation::Value::Kind::Integer:
        return Json {*value.getIf<std::int64_t>()};
    case foundation::Value::Kind::Number:
        return Json {*value.getIf<double>()};
    case foundation::Value::Kind::String:
        return Json {*value.getIf<std::string>()};
    case foundation::Value::Kind::Array: {
        Json result(jsoncons::json_array_arg);
        for(const auto& item : *value.getIf<foundation::Value::Array>()) {
            result.push_back(toJson(item));
        }
        return result;
    }
    case foundation::Value::Kind::Object: {
        Json result(jsoncons::json_object_arg);
        for(const auto& [key, item] : *value.getIf<foundation::Value::Object>()) {
            result.try_emplace(key, toJson(item));
        }
        return result;
    }
    }
    throw std::logic_error("Unknown Kernel Value kind");
}

foundation::Value fromJson(const Json& value)
{
    if(value.is_null()) {
        return foundation::Value {};
    }
    if(value.is_bool()) {
        return foundation::Value {value.as<bool>()};
    }
    if(value.is_int64()) {
        return foundation::Value {value.as<std::int64_t>()};
    }
    if(value.is_uint64()) {
        const auto number = value.as<std::uint64_t>();
        if(number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw std::range_error("JSON unsigned integer exceeds Kernel Value range");
        }
        return foundation::Value {static_cast<std::int64_t>(number)};
    }
    if(value.is_double()) {
        return foundation::Value {value.as<double>()};
    }
    if(value.is_string()) {
        return foundation::Value {value.as<std::string>()};
    }
    if(value.is_array()) {
        foundation::Value::Array result;
        result.reserve(value.size());
        for(const auto& item : value.array_range()) {
            result.push_back(fromJson(item));
        }
        return foundation::Value {std::move(result)};
    }
    if(value.is_object()) {
        foundation::Value::Object result;
        for(const auto& member : value.object_range()) {
            result.emplace(std::string(member.key()), fromJson(member.value()));
        }
        return foundation::Value {std::move(result)};
    }
    throw std::invalid_argument("Unsupported JSON value kind");
}

const char* schemaType(foundation::SchemaKind kind)
{
    switch(kind) {
    case foundation::SchemaKind::Null: return "null";
    case foundation::SchemaKind::Boolean: return "boolean";
    case foundation::SchemaKind::Integer: return "integer";
    case foundation::SchemaKind::Number: return "number";
    case foundation::SchemaKind::String: return "string";
    case foundation::SchemaKind::Array: return "array";
    case foundation::SchemaKind::Object: return "object";
    case foundation::SchemaKind::Any: return nullptr;
    }
    // Only explicit Any may omit the root type constraint.
    // 中文翻译：只有显式 Any 可以省略根类型约束，未知枚举不得弱化为任意类型。
    throw std::invalid_argument("Unknown Kernel Schema kind");
}

foundation::Error adapterError(const char* code, const char* message, const std::exception& exception)
{
    return foundation::makeError(
        code,
        foundation::ErrorCategory::Infrastructure,
        message,
        foundation::Value {foundation::Value::Object {
            {"backend", foundation::Value {"jsoncons"}},
            {"reason", foundation::Value {exception.what()}},
        }});
}

} // namespace

foundation::Result<std::string> JsonconsAdapter::serialize(const foundation::Value& value) const
{
    try {
        std::string output;
        jsoncons::encode_json(toJson(value), output);
        return foundation::Result<std::string>::success(std::move(output));
    } catch(const std::exception& exception) {
        return foundation::Result<std::string>::failure(
            adapterError("Serialization.JsonEncodeFailed", "JSON serialization failed", exception));
    }
}

foundation::Result<foundation::Value> JsonconsAdapter::deserialize(std::string_view payload) const
{
    try {
        return foundation::Result<foundation::Value>::success(
            fromJson(Json::parse(std::string(payload))));
    } catch(const std::exception& exception) {
        return foundation::Result<foundation::Value>::failure(
            adapterError("Serialization.JsonParseFailed", "JSON parsing failed", exception));
    }
}

foundation::Result<void> JsonconsAdapter::validate(
    const foundation::Schema& schema,
    const foundation::Value& value) const
{
    try {
        Json schemaJson = toJson(schema.constraints());
        if(const char* type = schemaType(schema.rootKind()); type != nullptr) {
            schemaJson.insert_or_assign("type", type);
        }
        const auto compiled = jsoncons::jsonschema::make_json_schema<Json>(std::move(schemaJson));
        if(!compiled.is_valid(toJson(value))) {
            return foundation::Result<void>::failure(foundation::makeError(
                "Runtime.SchemaInvalid",
                foundation::ErrorCategory::Validation,
                "The value does not satisfy the schema",
                foundation::Value {foundation::Value::Object {
                    {"schemaId", foundation::Value {std::string(schema.id().value())}},
                    {"schemaVersion", foundation::Value {schema.version().toString()}},
                }}));
        }
        return foundation::Result<void>::success();
    } catch(const std::exception& exception) {
        return foundation::Result<void>::failure(
            adapterError("Serialization.SchemaBackendFailed", "JSON Schema validation failed", exception));
    }
}

} // namespace lasercnc::infrastructure
