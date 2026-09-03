#pragma once

#include <compare>
#include <cstdint>
#include <string>

namespace lasercnc::foundation {

struct Version final {
    std::uint32_t major{0};
    std::uint32_t minor{0};
    std::uint32_t patch{0};

    [[nodiscard]] std::string toString() const;

    friend auto operator<=>(const Version&, const Version&) = default;
};

} // namespace lasercnc::foundation
