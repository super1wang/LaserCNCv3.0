#pragma once

#include <lasercnc/kernel/identifiers.hpp>
#include <array>
#include <cstdint>
#include <optional>

namespace lasercnc::runtime {
namespace detail { class CatalogClock; }

// Equality is meaningful for the entire token, never for revision alone.
// 中文翻译：只能比较完整令牌是否相等，不能仅比较修订数值。
struct CatalogVersion final {
    std::array<std::uint32_t, 4U> epoch{};
    std::optional<kernel::ProjectId> projectId;
    std::uint64_t revision{0U};
    friend bool operator==(const CatalogVersion&, const CatalogVersion&) = default;
};
} // namespace lasercnc::runtime
