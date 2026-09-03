#include "../../../src/runtime/persistence/object_record_codec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace lasercnc::foundation;
using namespace lasercnc::kernel;
using namespace lasercnc::state;
using namespace lasercnc::persistence::detail;

TEST_CASE("Object record codec preserves exact schema identity", "[persistence][object-type]")
{
    const ObjectRecord source {
        ObjectId::create("object.codec").value(), ObjectTypeId::create("type.codec").value(),
        Value {Value::Object {{"label", Value {"payload"}}}},
        Version {7U, 3U, std::numeric_limits<std::uint32_t>::max()}};
    const auto encoded = encodeObjectRecord(source);
    auto decoded = decodeObjectRecord(encoded, false);
    REQUIRE(decoded.hasValue());
    CHECK(decoded.value() == source);
    CHECK_FALSE(decodeObjectRecord(encoded, true).hasValue());

    auto legacy = encoded;
    legacy.getIf<Value::Object>()->erase("schemaVersion");
    CHECK_FALSE(decodeObjectRecord(legacy, false).hasValue());
    auto old = decodeObjectRecord(legacy, true);
    REQUIRE(old.hasValue());
    CHECK(old.value().schemaVersion == Version {1U, 0U, 0U});
    CHECK(old.value().data == source.data);
}

TEST_CASE("Object record codec rejects incomplete and malformed schema versions", "[persistence][object-type]")
{
    const ObjectRecord source {
        ObjectId::create("object.codec").value(), ObjectTypeId::create("type.codec").value(),
        Value {"data"}, Version {2U, 0U, 0U}};
    auto encoded = encodeObjectRecord(source);
    auto& root = *encoded.getIf<Value::Object>();
    auto& version = *root.at("schemaVersion").getIf<Value::Object>();
    SECTION("Missing version") { root.erase("schemaVersion"); }
    SECTION("Negative part") { version.at("minor") = Value {std::int64_t {-1}}; }
    SECTION("Overflow part") { version.at("major") = Value {std::int64_t {4294967296LL}}; }
    SECTION("String is not a version number") { version.at("patch") = Value {"0"}; }
    SECTION("Missing part") { version.erase("patch"); }
    SECTION("Unexpected part") { version.emplace("build", Value {std::int64_t {1}}); }
    SECTION("Unexpected record field") { root.emplace("guess", Value {true}); }
    SECTION("Missing data") { root.erase("data"); }
    SECTION("Invalid identity") { root.at("id") = Value {""}; }
    CHECK_FALSE(decodeObjectRecord(encoded, false).hasValue());
}
