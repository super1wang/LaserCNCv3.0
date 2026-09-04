#include <lasercnc/foundation/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>

using lasercnc::foundation::Value;
using lasercnc::foundation::ValueBudget;
using lasercnc::foundation::ValueBudgetViolation;
using lasercnc::foundation::assessValueBudget;
using lasercnc::foundation::kernelValueBudget;
using lasercnc::foundation::valueBudgetViolationName;

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

TEST_CASE("Value budget measures depth nodes and text without widening the kernel ceiling", "[foundation][value][budget][c6c1]")
{
    CHECK(kernelValueBudget.maximumDepth == 64U);
    CHECK(kernelValueBudget.maximumNodes == 100000U);
    CHECK(kernelValueBudget.maximumTextBytes == 16U * 1024U * 1024U);
    CHECK(kernelValueBudget.maximumEncodedBytes == 64U * 1024U * 1024U);

    const Value value {Value::Object {
        {"aa", Value {Value::Array {Value {"bbb"}}}},
    }};
    const ValueBudget exact {3U, 3U, 5U, 32U};
    const auto accepted = assessValueBudget(value, exact);
    REQUIRE(accepted.accepted());
    CHECK(accepted.maximumDepth == 3U);
    CHECK(accepted.nodes == 3U);
    CHECK(accepted.textBytes == 5U);

    auto depth = exact;
    depth.maximumDepth = 2U;
    CHECK(assessValueBudget(value, depth).violation == ValueBudgetViolation::Depth);
    auto nodes = exact;
    nodes.maximumNodes = 2U;
    CHECK(assessValueBudget(value, nodes).violation == ValueBudgetViolation::Nodes);
    auto text = exact;
    text.maximumTextBytes = 4U;
    CHECK(assessValueBudget(value, text).violation == ValueBudgetViolation::TextBytes);

    auto widened = kernelValueBudget;
    ++widened.maximumDepth;
    CHECK(assessValueBudget(value, widened).violation == ValueBudgetViolation::InvalidBudget);
    CHECK(std::string(valueBudgetViolationName(ValueBudgetViolation::InvalidBudget)) == "invalidBudget");
}
