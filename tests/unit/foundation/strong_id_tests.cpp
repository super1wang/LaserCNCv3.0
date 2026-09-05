#include <lasercnc/foundation/strong_id.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <type_traits>
#include <unordered_map>

namespace {

struct RequestIdTag;
struct SessionIdTag;

using RequestId = lasercnc::foundation::StrongId<RequestIdTag>;
using SessionId = lasercnc::foundation::StrongId<SessionIdTag>;

static_assert(!std::is_same_v<RequestId, SessionId>);
static_assert(!std::is_convertible_v<RequestId, SessionId>);

} // namespace

TEST_CASE("StrongId preserves type boundaries and stable strings", "[foundation][strong-id]")
{
    auto result = RequestId::create("request-0001");

    REQUIRE(result.hasValue());
    CHECK(std::string(result.value().value()) == "request-0001");

    std::unordered_map<RequestId, int, lasercnc::foundation::StrongIdHash<RequestIdTag>> values;
    values.emplace(result.value(), 42);
    CHECK(values.at(result.value()) == 42);
}

TEST_CASE("StrongId rejects blank and control characters", "[foundation][strong-id]")
{
    const auto empty = RequestId::create("");
    const auto blank = RequestId::create("   ");
    const auto control = RequestId::create(std::string("bad\nvalue"));
    const auto embeddedWhitespace = RequestId::create("request 1");

    REQUIRE_FALSE(empty.hasValue());
    REQUIRE_FALSE(blank.hasValue());
    REQUIRE_FALSE(control.hasValue());
    REQUIRE_FALSE(embeddedWhitespace.hasValue());
    CHECK(std::string(empty.error().code.value()) == "Foundation.InvalidStrongId");
    CHECK(empty.error().category == lasercnc::foundation::ErrorCategory::Validation);
}

TEST_CASE("StrongId rejects over-budget and malformed UTF-8 identities", "[foundation][strong-id][budget][c6c2a]")
{
    const auto exact = RequestId::create(std::string(4096U, 'a'));
    REQUIRE(exact);
    CHECK(exact.value().value().size() == 4096U);

    const auto oversized = RequestId::create(std::string(4097U, 'a'));
    REQUIRE_FALSE(oversized);
    CHECK(std::string(oversized.error().code.value())
          == "Foundation.StrongIdBudgetExceeded");

    const std::string malformed[] {
        std::string {"\xC3\x28", 2U},
        std::string {"\xC0\xAF", 2U},
        std::string {"\xED\xA0\x80", 3U},
        std::string {"\xF4\x90\x80\x80", 4U},
    };
    for(const auto& value : malformed) {
        const auto rejected = RequestId::create(value);
        REQUIRE_FALSE(rejected);
        CHECK(std::string(rejected.error().code.value())
              == "Foundation.InvalidStrongIdEncoding");
    }
}
