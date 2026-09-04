#include "catalog_clock.hpp"
#include <algorithm>
#include <random>
#include <stdexcept>

namespace lasercnc::runtime::detail {
CatalogClock::CatalogClock()
{
    // A cache namespace, not a security credential or a persisted business revision.
    // 中文翻译：这是缓存命名空间，不是安全凭据或持久业务修订。
    std::random_device source;
    std::uniform_int_distribution<std::uint32_t> bits;
    for(auto& part : epoch_) { part = bits(source); }
    if(std::all_of(epoch_.begin(), epoch_.end(), [](auto part) { return part == 0U; })) {
        throw std::runtime_error("Catalog epoch generation returned the reserved empty value");
    }
}

void CatalogClock::touch(const kernel::ProjectId& projectId)
{
    // Allocate before the caller changes visible state; retain scopes after removal.
    // 中文翻译：在调用方改变可见状态之前分配，删除条目后仍保留作用域计数。
    auto& scoped = projects_[projectId];
    scoped.advance();
    all_.advance();
}

foundation::Result<CatalogVersion> CatalogClock::version(
    const std::optional<kernel::ProjectId>& projectId) const
{
    const CatalogCounter empty;
    const auto found = projectId ? projects_.find(*projectId) : projects_.end();
    const auto& counter = !projectId ? all_ : found == projects_.end() ? empty : found->second;
    auto current = counter.current();
    if(!current) { return foundation::Result<CatalogVersion>::failure(std::move(current).error()); }
    return foundation::Result<CatalogVersion>::success({epoch_, projectId, current.value()});
}
} // namespace lasercnc::runtime::detail
