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
