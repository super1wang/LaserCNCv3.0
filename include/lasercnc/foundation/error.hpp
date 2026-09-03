#pragma once

#include <lasercnc/foundation/value.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace lasercnc::foundation {

class ErrorCode final {
public:
    explicit ErrorCode(std::string value);

    [[nodiscard]] std::string_view value() const noexcept;

    friend bool operator==(const ErrorCode&, const ErrorCode&) = default;

private:
    std::string value_;
};

enum class ErrorCategory : std::uint8_t {
    Validation,
    NotFound,
    Conflict,
    Authorization,
    Cancellation,
    Timeout,
    Infrastructure,
    Internal
};

enum class Severity : std::uint8_t {
    Info,
    Warning,
    Error,
    Fatal
};

struct Error final {
    ErrorCode code;
    ErrorCategory category{ErrorCategory::Internal};
    Severity severity{Severity::Error};
    std::string message;
    Value details;
    std::shared_ptr<const Error> cause;
};

[[nodiscard]] Error makeError(
    std::string code,
    ErrorCategory category,
    std::string message,
    Value details = Value {},
    Severity severity = Severity::Error,
    std::shared_ptr<const Error> cause = nullptr);

} // namespace lasercnc::foundation
