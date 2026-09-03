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

template <typename Tag>
class StrongId final {
public:
    [[nodiscard]] static Result<StrongId> create(std::string value)
    {
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
