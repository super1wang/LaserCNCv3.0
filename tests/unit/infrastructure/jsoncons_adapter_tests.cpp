#include <lasercnc/infrastructure/jsoncons_adapter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>

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

TEST_CASE("JsonconsAdapter rejects values beyond the kernel nesting budget", "[infrastructure][json][budget][c6c1]")
{
    JsonconsAdapter adapter;
    Value nested {"leaf"};
    std::string encoded {"0"};
    for(std::size_t depth = 1U; depth < kernelValueBudget.maximumDepth; ++depth) {
        nested = Value {Value::Array {std::move(nested)}};
        encoded.insert(encoded.begin(), '[');
        encoded.push_back(']');
    }
    REQUIRE(adapter.serialize(nested));
    REQUIRE(adapter.deserialize(encoded));

    nested = Value {Value::Array {std::move(nested)}};
    encoded.insert(encoded.begin(), '[');
    encoded.push_back(']');

    const auto serialized = adapter.serialize(nested);
    REQUIRE_FALSE(serialized.hasValue());
    CHECK(std::string(serialized.error().code.value()) == "Serialization.ValueBudgetExceeded");

    const auto parsed = adapter.deserialize(encoded);
    REQUIRE_FALSE(parsed.hasValue());
    CHECK(std::string(parsed.error().code.value()) == "Serialization.ValueBudgetExceeded");
}

TEST_CASE("JsonconsAdapter enforces node text and encoded byte budgets", "[infrastructure][json][budget][c6c1]")
{
    JsonconsAdapter adapter;

    const Value tooManyNodes {Value::Array(
        kernelValueBudget.maximumNodes,
        Value {})};
    const auto nodeWrite = adapter.serialize(tooManyNodes);
    REQUIRE_FALSE(nodeWrite);
    CHECK(std::string(nodeWrite.error().code.value()) == "Serialization.ValueBudgetExceeded");

    std::string nodePayload {"["};
    nodePayload.reserve(kernelValueBudget.maximumNodes * 2U + 1U);
    for(std::size_t index = 0U; index < kernelValueBudget.maximumNodes; ++index) {
        if(index != 0U) nodePayload.push_back(',');
        nodePayload.push_back('0');
    }
    nodePayload.push_back(']');
    const auto nodeRead = adapter.deserialize(nodePayload);
    REQUIRE_FALSE(nodeRead);
    CHECK(std::string(nodeRead.error().code.value()) == "Serialization.ValueBudgetExceeded");

    const Value tooMuchText {std::string(
        kernelValueBudget.maximumTextBytes + 1U,
        'x')};
    const auto textWrite = adapter.serialize(tooMuchText);
    REQUIRE_FALSE(textWrite);
    CHECK(std::string(textWrite.error().code.value()) == "Serialization.ValueBudgetExceeded");

    std::string oversizedInput(
        kernelValueBudget.maximumEncodedBytes + 1U,
        ' ');
    const auto byteRead = adapter.deserialize(oversizedInput);
    REQUIRE_FALSE(byteRead);
    CHECK(std::string(byteRead.error().code.value()) == "Serialization.InputBudgetExceeded");

    const Value escapedOutput {std::string(
        kernelValueBudget.maximumEncodedBytes / 6U + 1U,
        '\x01')};
    const auto byteWrite = adapter.serialize(escapedOutput);
    REQUIRE_FALSE(byteWrite);
    CHECK(std::string(byteWrite.error().code.value()) == "Serialization.OutputBudgetExceeded");
}

TEST_CASE("JsonconsAdapter applies the value budget before schema backend work", "[infrastructure][json][schema][budget][c6c1]")
{
    auto id = SchemaId::create("schema.budgeted-input");
    REQUIRE(id);
    auto schema = Schema::create(
        std::move(id).value(),
        Version {1U, 0U, 0U},
        SchemaKind::Any);
    REQUIRE(schema);
    Value nested {"leaf"};
    for(std::size_t depth = 0U; depth < kernelValueBudget.maximumDepth; ++depth) {
        nested = Value {Value::Array {std::move(nested)}};
    }
    const auto validated = JsonconsAdapter {}.validate(schema.value(), nested);
    REQUIRE_FALSE(validated);
    CHECK(std::string(validated.error().code.value()) == "Serialization.ValueBudgetExceeded");
}
