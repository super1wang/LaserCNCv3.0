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

TEST_CASE("JsonconsAdapter never receives an admitted unknown schema kind", "[infrastructure][json][schema][c6]")
{
    JsonconsAdapter adapter;
    for(const unsigned int raw : {8U, 255U}) {
        DYNAMIC_SECTION("rootKind=" << raw) {
            auto id = SchemaId::create("schema.unknown-admission");
            REQUIRE(id);
            auto schema = Schema::create(id.value(), Version{1U, 0U, 0U}, static_cast<SchemaKind>(raw));
            CHECK_FALSE(schema);
            if(schema) {
                // A regression must expose backend acceptance as well as factory admission.
                // 中文翻译：回归时同时暴露工厂错误准入与后端接纳，不能只记录构造成功。
                for(const auto& value : {Value{}, Value{true}, Value{std::int64_t{7}}, Value{2.5},
                        Value{"text"}, Value{Value::Array{}}, Value{Value::Object{}}}) {
                    INFO("valueKind=" << static_cast<unsigned int>(value.kind()));
                    CHECK_FALSE(adapter.validate(schema.value(), value));
                }
            } else {
                CHECK(std::string(schema.error().code.value()) == "Foundation.SchemaKindInvalid");
            }
        }
    }
}

TEST_CASE("JsonconsAdapter keeps explicit Any and all declared schema root semantics", "[infrastructure][json][schema][c6]")
{
    JsonconsAdapter adapter;
    const Value values[] = {Value{}, Value{true}, Value{std::int64_t{7}}, Value{2.5},
        Value{"text"}, Value{Value::Array{}}, Value{Value::Object{}}};
    const SchemaKind kinds[] = {SchemaKind::Any, SchemaKind::Null, SchemaKind::Boolean,
        SchemaKind::Integer, SchemaKind::Number, SchemaKind::String, SchemaKind::Array, SchemaKind::Object};
    for(unsigned int root = 0U; root < 8U; ++root) {
        DYNAMIC_SECTION("rootKind=" << root) {
            auto id = SchemaId::create("schema.declared-semantics");
            REQUIRE(id);
            auto schema = Schema::create(id.value(), Version{1U, 0U, 0U}, kinds[root]);
            REQUIRE(schema);
            for(unsigned int input = 0U; input < 7U; ++input) {
                INFO("valueKind=" << input);
                const bool expected = root == 0U || root == input + 1U || (root == 4U && input == 2U);
                const auto result = adapter.validate(schema.value(), values[input]);
                CHECK(result.hasValue() == expected);
                if(!expected && !result) {
                    CHECK(std::string(result.error().code.value()) == "Runtime.SchemaInvalid");
                }
            }
        }
    }
    auto id = SchemaId::create("schema.explicit-any-constrained");
    REQUIRE(id);
    auto constrained = Schema::create(id.value(), Version{1U, 0U, 0U}, SchemaKind::Any,
        Value{Value::Object{{"type", Value{"integer"}}, {"minimum", Value{std::int64_t{1}}}}});
    REQUIRE(constrained);
    CHECK(adapter.validate(constrained.value(), Value{std::int64_t{2}}));
    CHECK_FALSE(adapter.validate(constrained.value(), Value{std::int64_t{0}}));
    CHECK_FALSE(adapter.validate(constrained.value(), Value{"text"}));
}
