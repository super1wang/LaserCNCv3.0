#include <lasercnc/foundation/result.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>

using namespace lasercnc::foundation;

TEST_CASE("Result separates success values from unified errors", "[foundation][result]")
{
    auto success = Result<std::int64_t>::success(17);
    auto failure = Result<std::int64_t>::failure(makeError(
        "Foundation.InvalidInput",
        ErrorCategory::Validation,
        "输入无效",
        Value {Value::Object {{"field", Value {"name"}}}}));

    REQUIRE(success.hasValue());
    CHECK(success.value() == 17);

    REQUIRE_FALSE(failure.hasValue());
    CHECK(std::string(failure.error().code.value()) == "Foundation.InvalidInput");
    CHECK(failure.error().details.kind() == Value::Kind::Object);
}

TEST_CASE("Error retains an immutable cause chain", "[foundation][error]")
{
    auto cause = std::make_shared<const Error>(makeError(
        "Infrastructure.BackendFailed",
        ErrorCategory::Infrastructure,
        "后端失败"));
    auto error = makeError(
        "Foundation.OperationFailed",
        ErrorCategory::Internal,
        "操作失败",
        Value {},
        Severity::Error,
        cause);

    REQUIRE(error.cause != nullptr);
    CHECK(std::string(error.cause->code.value()) == "Infrastructure.BackendFailed");
}

TEST_CASE("Result void supports value-less success", "[foundation][result]")
{
    auto success = Result<void>::success();
    auto failure = Result<void>::failure(makeError(
        "Foundation.Rejected",
        ErrorCategory::Conflict,
        "请求被拒绝"));

    CHECK(success.hasValue());
    REQUIRE_FALSE(failure.hasValue());
    CHECK(failure.error().category == ErrorCategory::Conflict);
}
