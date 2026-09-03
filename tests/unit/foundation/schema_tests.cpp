#include <lasercnc/foundation/schema.hpp>

#include <catch2/catch_test_macros.hpp>

#include <compare>
#include <cstdint>
#include <string>

using namespace lasercnc::foundation;

TEST_CASE("Version provides stable semantic ordering", "[foundation][version]")
{
    const Version current {1, 2, 3};
    const Version next {1, 3, 0};

    CHECK(current.toString() == "1.2.3");
    CHECK(current < next);
}

TEST_CASE("Schema owns kernel values and unit metadata only", "[foundation][schema]")
{
    auto id = SchemaId::create("schema.command.project-new.arguments");
    REQUIRE(id.hasValue());

    auto schemaResult = Schema::create(
        std::move(id).value(),
        Version {1, 0, 0},
        SchemaKind::Object,
        Value {Value::Object {
            {"required", Value {Value::Array {Value {"name"}}}},
        }},
        "mm");

    REQUIRE(schemaResult.hasValue());
    const auto& schema = schemaResult.value();

    CHECK(std::string(schema.id().value()) == "schema.command.project-new.arguments");
    CHECK(schema.version() == Version {1, 0, 0});
    CHECK(schema.rootKind() == SchemaKind::Object);
    CHECK(schema.constraints().kind() == Value::Kind::Object);
    REQUIRE(schema.unit().has_value());
    CHECK(*schema.unit() == "mm");
}

TEST_CASE("Schema rejects invalid constraint and unit metadata", "[foundation][schema]")
{
    auto firstId = SchemaId::create("schema.invalid.constraints");
    auto secondId = SchemaId::create("schema.invalid.unit");
    REQUIRE(firstId.hasValue());
    REQUIRE(secondId.hasValue());

    auto badConstraints = Schema::create(
        std::move(firstId).value(),
        Version {1, 0, 0},
        SchemaKind::String,
        Value {"not-an-object"});
    auto badUnit = Schema::create(
        std::move(secondId).value(),
        Version {1, 0, 0},
        SchemaKind::Number,
        Value {Value::Object {}},
        "   ");

    REQUIRE_FALSE(badConstraints.hasValue());
    CHECK(std::string(badConstraints.error().code.value()) == "Foundation.SchemaConstraintsInvalid");
    REQUIRE_FALSE(badUnit.hasValue());
    CHECK(std::string(badUnit.error().code.value()) == "Foundation.SchemaUnitInvalid");
}
