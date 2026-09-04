#pragma once

#include <lasercnc/runtime/lifecycle_catalog.hpp>
#include <lasercnc/foundation/result.hpp>
#include <limits>
#include <map>

namespace lasercnc::runtime::detail {
// Internal counter; exhaustion disables token publication instead of wrapping.
// 中文翻译：内部计数耗尽后停止发布令牌，绝不回绕成旧版本。
struct CatalogCounter final {
    std::uint64_t value{0U};
    bool exhausted{false};
    void advance() noexcept
    {
        if(value == std::numeric_limits<std::uint64_t>::max()) { exhausted = true; }
        else { ++value; }
    }
    [[nodiscard]] foundation::Result<std::uint64_t> current() const
    {
        if(exhausted) {
            return foundation::Result<std::uint64_t>::failure(foundation::makeError(
                "Catalog.RevisionExhausted", foundation::ErrorCategory::Infrastructure,
                "The catalog invalidation counter is exhausted; no reusable version can be published"));
        }
        return foundation::Result<std::uint64_t>::success(value);
    }
};

class CatalogClock final {
public:
    CatalogClock();
    void touch(const kernel::ProjectId& projectId);
    [[nodiscard]] foundation::Result<CatalogVersion> version(
        const std::optional<kernel::ProjectId>& projectId) const;
private:
    std::array<std::uint32_t, 4U> epoch_{};
    CatalogCounter all_;
    std::map<kernel::ProjectId, CatalogCounter> projects_;
};
} // namespace lasercnc::runtime::detail
