#include <lasercnc/infrastructure/toml_config_adapter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>

using namespace lasercnc::foundation;
using lasercnc::infrastructure::TomlConfigAdapter;

TEST_CASE("TomlConfigAdapter parses and serializes human configuration", "[infrastructure][toml]")
{
    TomlConfigAdapter adapter;
    auto parsed = adapter.parse(
        "title = \"LaserCNC\"\n[machine]\naxes = 5\nenabled = true\n",
        "test.toml");
    REQUIRE(parsed.hasValue());
    REQUIRE(parsed.value().kind() == Value::Kind::Object);

    const auto* root = parsed.value().getIf<Value::Object>();
    REQUIRE(root != nullptr);
    CHECK(*root->at("title").getIf<std::string>() == "LaserCNC");

    auto serialized = adapter.serialize(parsed.value());
    REQUIRE(serialized.hasValue());
    auto reparsed = adapter.parse(serialized.value(), "roundtrip.toml");
    REQUIRE(reparsed.hasValue());
    CHECK(reparsed.value() == parsed.value());
}

TEST_CASE("TomlConfigAdapter converts syntax and unsupported value errors", "[infrastructure][toml]")
{
    TomlConfigAdapter adapter;
    auto malformed = adapter.parse("name = [", "broken.toml");
    REQUIRE_FALSE(malformed.hasValue());
    CHECK(std::string(malformed.error().code.value()) == "Config.ParseFailed");

    auto date = adapter.parse("created = 2026-09-03", "date.toml");
    REQUIRE_FALSE(date.hasValue());
    CHECK(std::string(date.error().code.value()) == "Config.ParseFailed");

    auto nullRoot = adapter.serialize(Value {});
    REQUIRE_FALSE(nullRoot.hasValue());
    CHECK(std::string(nullRoot.error().code.value()) == "Config.RootNotObject");

    auto nestedNull = adapter.serialize(Value {Value::Object {{"missing", Value {}}}});
    REQUIRE_FALSE(nestedNull.hasValue());
    CHECK(std::string(nestedNull.error().code.value()) == "Config.SerializeFailed");
}

TEST_CASE("TomlConfigAdapter rejects values beyond the kernel nesting budget", "[infrastructure][toml][budget][c6c1]")
{
    TomlConfigAdapter adapter;
    Value nested {"leaf"};
    for(std::size_t depth = 1U; depth < kernelValueBudget.maximumDepth; ++depth) {
        nested = Value {Value::Object {{"level", std::move(nested)}}};
    }
    REQUIRE(adapter.serialize(nested));
    nested = Value {Value::Object {{"level", std::move(nested)}}};
    const auto serialized = adapter.serialize(nested);
    REQUIRE_FALSE(serialized.hasValue());
    CHECK(std::string(serialized.error().code.value()) == "Config.ValueBudgetExceeded");

    std::string path {"level"};
    for(std::size_t depth = 1U; depth < 63U; ++depth) path += ".level";
    const std::string content = "[" + path + "]\nvalue = 1\n";
    const auto parsed = adapter.parse(content, "deep.toml");
    REQUIRE_FALSE(parsed.hasValue());
    CHECK(std::string(parsed.error().code.value()) == "Config.ValueBudgetExceeded");
}

TEST_CASE("TomlConfigAdapter enforces node text and encoded input budgets", "[infrastructure][toml][budget][c6c1]")
{
    TomlConfigAdapter adapter;

    Value::Array values(kernelValueBudget.maximumNodes, Value {});
    const Value tooManyNodes {Value::Object {{"values", Value {std::move(values)}}}};
    const auto nodeWrite = adapter.serialize(tooManyNodes);
    REQUIRE_FALSE(nodeWrite);
    CHECK(std::string(nodeWrite.error().code.value()) == "Config.ValueBudgetExceeded");

    const Value tooMuchText {Value::Object {{
        "value",
        Value {std::string(kernelValueBudget.maximumTextBytes + 1U, 'x')},
    }}};
    const auto textWrite = adapter.serialize(tooMuchText);
    REQUIRE_FALSE(textWrite);
    CHECK(std::string(textWrite.error().code.value()) == "Config.ValueBudgetExceeded");

    std::string oversizedInput(
        kernelValueBudget.maximumEncodedBytes + 1U,
        ' ');
    const auto byteRead = adapter.parse(oversizedInput, "oversized.toml");
    REQUIRE_FALSE(byteRead);
    CHECK(std::string(byteRead.error().code.value()) == "Config.InputBudgetExceeded");

    const auto sourceRead = adapter.parse(
        "value = 1\n",
        std::string(kernelValueBudget.maximumTextBytes + 1U, 's'));
    REQUIRE_FALSE(sourceRead);
    CHECK(std::string(sourceRead.error().code.value()) == "Config.SourceNameBudgetExceeded");

    const Value escapedOutput {Value::Object {{
        "value",
        Value {std::string(
            kernelValueBudget.maximumEncodedBytes / 6U + 1U,
            '\x01')},
    }}};
    const auto byteWrite = adapter.serialize(escapedOutput);
    REQUIRE_FALSE(byteWrite);
    CHECK(std::string(byteWrite.error().code.value()) == "Config.OutputBudgetExceeded");
}
