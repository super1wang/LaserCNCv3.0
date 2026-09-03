#include <lasercnc/infrastructure/jsoncons_adapter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

using namespace lasercnc::foundation;
using lasercnc::infrastructure::JsonconsAdapter;

TEST_CASE("JsonconsAdapter round trips every Kernel Value kind", "[infrastructure][json]")
{
    const Value original {Value::Object {
        {"array", Value {Value::Array {Value {}, Value {true}, Value {std::int64_t {7}}, Value {2.5}}}},
        {"name", Value {"demo"}},
        {"nested", Value {Value::Object {{"enabled", Value {false}}}}},
    }};
    JsonconsAdapter adapter;

    auto encoded = adapter.serialize(original);
    REQUIRE(encoded.hasValue());
    auto decoded = adapter.deserialize(encoded.value());
    REQUIRE(decoded.hasValue());
    CHECK(decoded.value() == original);
}

TEST_CASE("JsonconsAdapter converts parse and range errors", "[infrastructure][json]")
{
    JsonconsAdapter adapter;
    auto malformed = adapter.deserialize("{broken");
    REQUIRE_FALSE(malformed.hasValue());
    CHECK(std::string(malformed.error().code.value()) == "Serialization.JsonParseFailed");

    auto overflow = adapter.deserialize("18446744073709551615");
    REQUIRE_FALSE(overflow.hasValue());
    CHECK(std::string(overflow.error().code.value()) == "Serialization.JsonParseFailed");
}

TEST_CASE("JsonconsAdapter validates Kernel Schema without exposing jsoncons", "[infrastructure][json][schema]")
{
    auto id = SchemaId::create("schema.test.positive-integer");
    REQUIRE(id.hasValue());
    auto schema = Schema::create(
        std::move(id).value(),
        Version {1, 0, 0},
        SchemaKind::Integer,
        Value {Value::Object {{"minimum", Value {std::int64_t {1}}}}});
    REQUIRE(schema.hasValue());

    JsonconsAdapter adapter;
    CHECK(adapter.validate(schema.value(), Value {std::int64_t {2}}).hasValue());
    auto invalid = adapter.validate(schema.value(), Value {std::int64_t {0}});
    REQUIRE_FALSE(invalid.hasValue());
    CHECK(std::string(invalid.error().code.value()) == "Runtime.SchemaInvalid");
}
