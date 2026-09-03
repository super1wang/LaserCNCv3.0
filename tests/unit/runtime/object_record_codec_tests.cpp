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
    auto decoded = decodeObjectRecord(encoded, ObjectRecordFormat::Assets);
    REQUIRE(decoded.hasValue());
    CHECK(decoded.value() == source);
    CHECK_FALSE(decodeObjectRecord(encoded, ObjectRecordFormat::Legacy).hasValue());

    auto legacy = encoded;
    legacy.getIf<Value::Object>()->erase("assets");
    auto versioned = decodeObjectRecord(legacy, ObjectRecordFormat::Versioned);
    REQUIRE(versioned.hasValue());
    CHECK(versioned.value() == source);
    legacy.getIf<Value::Object>()->erase("schemaVersion");
    CHECK_FALSE(decodeObjectRecord(legacy, ObjectRecordFormat::Assets).hasValue());
    auto old = decodeObjectRecord(legacy, ObjectRecordFormat::Legacy);
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
    CHECK_FALSE(decodeObjectRecord(encoded, ObjectRecordFormat::Assets).hasValue());
}

TEST_CASE("Object record codec preserves assets and rejects ambiguous asset metadata", "[persistence][asset]")
{
    const ObjectRecord source{ObjectId::create("object.codec").value(), ObjectTypeId::create("type.codec").value(),
        Value{}, Version{2U, 0U, 0U}, {{AssetId::create("asset.codec").value(),
            ContentDigest::create("digest.codec").value(), AssetKind::create("test.binary").value(),
            std::numeric_limits<std::uint64_t>::max()}}};
    auto encoded = encodeObjectRecord(source);
    auto decoded = decodeObjectRecord(encoded, ObjectRecordFormat::Assets);
    REQUIRE(decoded.hasValue());
    CHECK(decoded.value() == source);
    CHECK_FALSE(decodeObjectRecord(encoded, ObjectRecordFormat::Versioned).hasValue());
    auto& root = *encoded.getIf<Value::Object>();
    auto& reference = *root.at("assets").getIf<Value::Array>()->front().getIf<Value::Object>();
    SECTION("missing assets") { root.erase("assets"); }
    SECTION("non-array assets") { root.at("assets") = Value{}; }
    SECTION("missing digest") { reference.erase("digest"); }
    SECTION("unknown field") { reference.emplace("path", Value{"untrusted"}); }
    SECTION("negative size") { reference.at("byteSize") = Value{"-1"}; }
    SECTION("overflow size") { reference.at("byteSize") = Value{"18446744073709551616"}; }
    SECTION("noncanonical size") { reference.at("byteSize") = Value{"01"}; }
    SECTION("number instead of canonical string") { reference.at("byteSize") = Value{std::int64_t{1}}; }
    SECTION("blank identity") { reference.at("id") = Value{""}; }
    CHECK_FALSE(decodeObjectRecord(encoded, ObjectRecordFormat::Assets).hasValue());
}
