#include <lasercnc/infrastructure/toml_config_adapter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

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
