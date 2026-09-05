#pragma once

#include <lasercnc/foundation/result.hpp>

#include <algorithm>
#include <cctype>
#include <compare>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace lasercnc::foundation {
namespace detail {

inline bool isUtf8Continuation(unsigned char byte) noexcept
{
    return (byte & 0xC0U) == 0x80U;
}

inline bool isWellFormedUtf8(std::string_view value) noexcept
{
    std::size_t index = 0U;
    while(index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if(first <= 0x7FU) {
            ++index;
            continue;
        }
        if(first >= 0xC2U && first <= 0xDFU) {
            if(index + 1U >= value.size()
               || !isUtf8Continuation(static_cast<unsigned char>(value[index + 1U]))) {
                return false;
            }
            index += 2U;
            continue;
        }
        if(first >= 0xE0U && first <= 0xEFU) {
            if(index + 2U >= value.size()) return false;
            const auto second = static_cast<unsigned char>(value[index + 1U]);
            const auto third = static_cast<unsigned char>(value[index + 2U]);
            const bool validSecond = first == 0xE0U ? second >= 0xA0U && second <= 0xBFU
                : first == 0xEDU ? second >= 0x80U && second <= 0x9FU
                                 : isUtf8Continuation(second);
            if(!validSecond || !isUtf8Continuation(third)) return false;
            index += 3U;
            continue;
        }
        if(first >= 0xF0U && first <= 0xF4U) {
            if(index + 3U >= value.size()) return false;
            const auto second = static_cast<unsigned char>(value[index + 1U]);
            const auto third = static_cast<unsigned char>(value[index + 2U]);
            const auto fourth = static_cast<unsigned char>(value[index + 3U]);
            const bool validSecond = first == 0xF0U ? second >= 0x90U && second <= 0xBFU
                : first == 0xF4U ? second >= 0x80U && second <= 0x8FU
                                 : isUtf8Continuation(second);
            if(!validSecond || !isUtf8Continuation(third) || !isUtf8Continuation(fourth)) {
                return false;
            }
            index += 4U;
            continue;
        }
        return false;
    }
    return true;
}

} // namespace detail

inline constexpr std::size_t kernelStrongIdMaximumBytes = 4096U;

template <typename Tag>
class StrongId final {
public:
    [[nodiscard]] static Result<StrongId> create(std::string value)
    {
        if(value.size() > kernelStrongIdMaximumBytes) {
            return Result<StrongId>::failure(makeError(
                "Foundation.StrongIdBudgetExceeded",
                ErrorCategory::Validation,
                "StrongId exceeds the Kernel identity byte budget",
                Value {Value::Object {
                    {"actual", Value {std::to_string(value.size())}},
                    {"dimension", Value {"identityBytes"}},
                    {"limit", Value {std::to_string(kernelStrongIdMaximumBytes)}},
                    {"material", Value {"strongId"}},
                }}));
        }
        if(!detail::isWellFormedUtf8(value)) {
            return Result<StrongId>::failure(makeError(
                "Foundation.InvalidStrongIdEncoding",
                ErrorCategory::Validation,
                "StrongId must contain well-formed UTF-8"));
        }
        const auto invalidCharacter = std::find_if(
            value.begin(),
            value.end(),
            [](unsigned char character) {
                return std::iscntrl(character) != 0 || std::isspace(character) != 0;
            });

        if(value.empty() || invalidCharacter != value.end()) {
            return Result<StrongId>::failure(makeError(
                "Foundation.InvalidStrongId",
                ErrorCategory::Validation,
                "StrongId must be non-empty and contain no whitespace or control characters"));
        }

        return Result<StrongId>::success(StrongId(std::move(value)));
    }

    [[nodiscard]] std::string_view value() const noexcept
    {
        return value_;
    }

    friend auto operator<=>(const StrongId&, const StrongId&) = default;

private:
    explicit StrongId(std::string value)
        : value_(std::move(value))
    {
    }

    std::string value_;
};

template <typename Tag>
struct StrongIdHash final {
    [[nodiscard]] std::size_t operator()(const StrongId<Tag>& id) const noexcept
    {
        return std::hash<std::string_view> {}(id.value());
    }
};

} // namespace lasercnc::foundation
