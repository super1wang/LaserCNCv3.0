#include <lasercnc/infrastructure/sha256_hash_service.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

using namespace lasercnc::infrastructure;

namespace {

std::span<const std::byte> bytes(std::string_view value)
{
    return {
        reinterpret_cast<const std::byte*>(value.data()),
        value.size()};
}

} // namespace

TEST_CASE("Sha256HashService produces stable strong content digests", "[infrastructure][hash]")
{
    Sha256HashService hashes;
    auto empty = hashes.digest({});
    REQUIRE(empty.hasValue());
    CHECK(std::string(empty.value().value())
          == "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    auto abc = hashes.digest(bytes("abc"));
    REQUIRE(abc.hasValue());
    CHECK(std::string(abc.value().value())
          == "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hashes.digest(bytes("abc")).value() == abc.value());
    CHECK(hashes.digest(bytes("abd")).value() != abc.value());
}
