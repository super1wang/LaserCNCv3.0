#include <lasercnc/observability/metrics_service.hpp>

#include <lasercnc/foundation/error.hpp>

#include "metric_accumulation.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <utility>

namespace lasercnc::observability {
namespace {

foundation::Error metricError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const kernel::MetricName& name)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {foundation::Value::Object {
            {"metric", foundation::Value {std::string(name.value())}},
        }});
}

} // namespace

struct LocalMetricsService::Impl final {
    Impl(std::size_t series, std::size_t failures)
        : seriesCapacity(std::max<std::size_t>(series, 1U)),
          failureCapacity(std::max<std::size_t>(failures, 1U))
    {
    }

    struct Key final {
        kernel::MetricName name;
        MetricLabels labels;
        friend auto operator<=>(const Key&, const Key&) = default;
    };
    using Aggregate = detail::MetricAggregate;

    std::mutex mutex;
    std::map<Key, Aggregate> values;
    std::vector<std::shared_ptr<IMetricsExporter>> exporters;
    std::vector<foundation::Error> failures;
    std::size_t seriesCapacity;
    std::size_t failureCapacity;
    bool frozen{false};
};

LocalMetricsService::LocalMetricsService(
    std::size_t seriesCapacity,
    std::size_t failureCapacity)
    : impl_(std::make_unique<Impl>(seriesCapacity, failureCapacity))
{
}
LocalMetricsService::~LocalMetricsService() = default;

foundation::Result<void> LocalMetricsService::addExporter(
    std::shared_ptr<IMetricsExporter> exporter)
{
    if(exporter == nullptr) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Metrics.InvalidExporter",
            foundation::ErrorCategory::Validation,
            "A metrics exporter is required"));
    }
    std::lock_guard lock(impl_->mutex);
    if(impl_->frozen) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Metrics.ExportersFrozen",
            foundation::ErrorCategory::Conflict,
            "Metrics exporters are frozen"));
    }
    impl_->exporters.push_back(std::move(exporter));
    return foundation::Result<void>::success();
}

void LocalMetricsService::freeze()
{
    std::lock_guard lock(impl_->mutex);
    impl_->frozen = true;
}

bool LocalMetricsService::frozen() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->frozen;
}

foundation::Result<void> LocalMetricsService::record(
    kernel::MetricName name,
    MetricKind kind,
    double value,
    MetricLabels labels)
{
    if(!std::isfinite(value) || (kind == MetricKind::Counter && value < 0.0)) {
        return foundation::Result<void>::failure(metricError(
            "Metrics.InvalidValue",
            foundation::ErrorCategory::Validation,
            "Metric values must be finite and counter deltas cannot be negative",
            name));
    }

    MetricObservation observation {
        name, kind, value, labels, std::chrono::system_clock::now()};
    std::vector<std::shared_ptr<IMetricsExporter>> exporters;
    {
        std::lock_guard lock(impl_->mutex);
        const Impl::Key key {name, labels};
        auto found = impl_->values.find(key);
        if(found != impl_->values.end() && found->second.kind != kind) {
            return foundation::Result<void>::failure(metricError(
                "Metrics.KindConflict",
                foundation::ErrorCategory::Conflict,
                "A metric identity cannot change kind",
                name));
        }
        if(found == impl_->values.end()) {
            if(impl_->values.size() >= impl_->seriesCapacity) {
                return foundation::Result<void>::failure(metricError(
                    "Metrics.SeriesCapacityExceeded",
                    foundation::ErrorCategory::Conflict,
                    "The bounded metric series capacity is exhausted",
                    name));
            }
        }
        auto next = found == impl_->values.end() ? Impl::Aggregate{kind} : found->second;
        if(!detail::tryAccumulateMetric(next, value)) {
            return foundation::Result<void>::failure(metricError(
                "Metrics.AggregateOverflow",
                foundation::ErrorCategory::Validation,
                "Metric aggregate sum and count must remain representable",
                name));
        }
        exporters = impl_->exporters;
        if(found == impl_->values.end()) { impl_->values.emplace(key, next); }
        else { found->second = next; }
    }

    const auto retainFailure = [this](foundation::Error failure) noexcept {
        try {
            std::lock_guard lock(impl_->mutex);
            if(impl_->failures.size() >= impl_->failureCapacity) {
                impl_->failures.erase(impl_->failures.begin());
            }
            impl_->failures.push_back(std::move(failure));
        } catch(...) {
        }
    };
    for(const auto& exporter : exporters) {
        try {
            auto exported = exporter->exportObservation(observation);
            if(!exported) {
                retainFailure(std::move(exported).error());
            }
        } catch(const std::exception& exception) {
            try {
                retainFailure(metricError(
                    "Metrics.ExporterThrew",
                    foundation::ErrorCategory::Internal,
                    exception.what(),
                    name));
            } catch(...) {
            }
        } catch(...) {
            try {
                retainFailure(metricError(
                    "Metrics.ExporterThrew",
                    foundation::ErrorCategory::Internal,
                    "A metrics exporter raised an unknown exception",
                    name));
            } catch(...) {
            }
        }
    }
    return foundation::Result<void>::success();
}

foundation::Result<void> LocalMetricsService::addCounter(
    kernel::MetricName name,
    double delta,
    MetricLabels labels)
{
    return record(std::move(name), MetricKind::Counter, delta, std::move(labels));
}

foundation::Result<void> LocalMetricsService::setGauge(
    kernel::MetricName name,
    double value,
    MetricLabels labels)
{
    return record(std::move(name), MetricKind::Gauge, value, std::move(labels));
}

foundation::Result<void> LocalMetricsService::observeHistogram(
    kernel::MetricName name,
    double value,
    MetricLabels labels)
{
    return record(std::move(name), MetricKind::Histogram, value, std::move(labels));
}

std::vector<MetricSnapshot> LocalMetricsService::snapshot() const
{
    std::lock_guard lock(impl_->mutex);
    std::vector<MetricSnapshot> result;
    result.reserve(impl_->values.size());
    for(const auto& [key, aggregate] : impl_->values) {
        result.push_back(MetricSnapshot {
            key.name,
            aggregate.kind,
            key.labels,
            aggregate.count,
            aggregate.value,
            aggregate.sum,
            aggregate.minimum,
            aggregate.maximum});
    }
    return result;
}

std::vector<foundation::Error> LocalMetricsService::exporterFailures() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->failures;
}

} // namespace lasercnc::observability
