#include <lasercnc/foundation/error.hpp>

#include <utility>

namespace lasercnc::foundation {

ErrorCode::ErrorCode(std::string value)
    : value_(std::move(value))
{
}

std::string_view ErrorCode::value() const noexcept
{
    return value_;
}

Error makeError(
    std::string code,
    ErrorCategory category,
    std::string message,
    Value details,
    Severity severity,
    std::shared_ptr<const Error> cause)
{
    return Error {
        ErrorCode {std::move(code)},
        category,
        severity,
        std::move(message),
        std::move(details),
        std::move(cause)};
}

} // namespace lasercnc::foundation
