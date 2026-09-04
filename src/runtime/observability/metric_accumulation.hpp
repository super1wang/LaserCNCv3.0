#pragma once

#include <lasercnc/observability/metrics_service.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lasercnc::observability::detail {

struct MetricAggregate final {
    MetricKind kind;
    std::uint64_t count{0U};
    double value{0.0};
    double sum{0.0};
    double minimum{0.0};
    double maximum{0.0};
};

// Prepare finite aggregation before changing any field; this is not an input budget.
// 中文翻译：任何字段变更前先检查聚合有限性和计数，不将数值检查当作输入预算。
[[nodiscard]] inline bool tryAccumulateMetric(MetricAggregate& aggregate, double value) noexcept
{
    if(aggregate.count == std::numeric_limits<std::uint64_t>::max()) { return false; }
    const double sum = aggregate.sum + value;
    if(!std::isfinite(sum)) { return false; }
    aggregate.minimum = aggregate.count == 0U ? value : std::min(aggregate.minimum, value);
    aggregate.maximum = aggregate.count == 0U ? value : std::max(aggregate.maximum, value);
    ++aggregate.count;
    aggregate.sum = sum;
    aggregate.value = aggregate.kind == MetricKind::Counter ? sum : value;
    return true;
}

} // namespace lasercnc::observability::detail
