#include <lasercnc/foundation/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

using lasercnc::foundation::Value;

TEST_CASE("Value covers every kernel value type", "[foundation][value]")
{
    const char* missingString = nullptr;

    CHECK(Value {}.kind() == Value::Kind::Null);
    CHECK(Value {missingString}.kind() == Value::Kind::Null);
    CHECK(Value {true}.kind() == Value::Kind::Boolean);
    CHECK(Value {std::int64_t {7}}.kind() == Value::Kind::Integer);
    CHECK(Value {3.5}.kind() == Value::Kind::Number);
    CHECK(Value {"laser"}.kind() == Value::Kind::String);
    CHECK(Value {Value::Array {}}.kind() == Value::Kind::Array);
    CHECK(Value {Value::Object {}}.kind() == Value::Kind::Object);
}

TEST_CASE("Value supports nesting without exposing a serializer", "[foundation][value]")
{
    Value object {Value::Object {
        {"name", Value {"demo"}},
        {"revision", Value {std::int64_t {12}}},
        {"flags", Value {Value::Array {Value {true}, Value {false}}}},
    }};

    const auto* fields = object.getIf<Value::Object>();
    REQUIRE(fields != nullptr);
    CHECK(fields->at("name").getIf<std::string>() != nullptr);
    CHECK(*fields->at("name").getIf<std::string>() == "demo");
    CHECK(object == Value {*fields});
}
