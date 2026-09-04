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

TEST_CASE("Schema rejects every undefined root kind before other metadata", "[foundation][schema][c6]")
{
    for(unsigned int raw = 8U; raw <= 255U; ++raw) {
        DYNAMIC_SECTION("rootKind=" << raw) {
            auto id = SchemaId::create("schema.unknown-kind");
            REQUIRE(id);
            for(const bool invalidMetadata : {false, true}) {
                INFO("invalidMetadata=" << invalidMetadata);
                auto schema = Schema::create(id.value(), Version{1U, 0U, 0U},
                    static_cast<SchemaKind>(raw), invalidMetadata ? Value{"not-object"} : Value{Value::Object{}},
                    invalidMetadata ? std::optional<std::string>{"   "} : std::nullopt);
                CHECK_FALSE(schema);
                if(!schema) {
                    CHECK(std::string(schema.error().code.value()) == "Foundation.SchemaKindInvalid");
                    CHECK(schema.error().category == ErrorCategory::Validation);
                }
            }
        }
    }
}

TEST_CASE("Schema preserves every declared root kind without changing metadata", "[foundation][schema][c6]")
{
    for(const auto kind : {SchemaKind::Any, SchemaKind::Null, SchemaKind::Boolean,
            SchemaKind::Integer, SchemaKind::Number, SchemaKind::String, SchemaKind::Array, SchemaKind::Object}) {
        DYNAMIC_SECTION("rootKind=" << static_cast<unsigned int>(kind)) {
            auto id = SchemaId::create("schema.known-kind");
            REQUIRE(id);
            const Value constraints{Value::Object{{"description", Value{"unchanged"}}}};
            auto schema = Schema::create(id.value(), Version{2U, 3U, 4U}, kind, constraints, "mm");
            REQUIRE(schema);
            CHECK(schema.value().id() == id.value());
            CHECK(schema.value().version() == Version{2U, 3U, 4U});
            CHECK(schema.value().rootKind() == kind);
            CHECK(schema.value().constraints() == constraints);
            CHECK(schema.value().unit() == std::optional<std::string>{"mm"});
        }
    }
}
