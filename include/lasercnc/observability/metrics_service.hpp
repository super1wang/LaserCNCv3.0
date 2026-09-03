#pragma once

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/value.hpp>
#include <lasercnc/kernel/identifiers.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lasercnc::observability {

enum class MetricKind : std::uint8_t { Counter, Gauge, Histogram };
using MetricLabels = std::map<std::string, std::string, std::less<>>;

struct MetricObservation final {
    kernel::MetricName name;
    MetricKind kind{MetricKind::Counter};
    double value{0.0};
    MetricLabels labels;
    std::chrono::system_clock::time_point timestamp;
};

struct MetricSnapshot final {
    kernel::MetricName name;
    MetricKind kind{MetricKind::Counter};
    MetricLabels labels;
    std::uint64_t count{0U};
    double value{0.0};
    double sum{0.0};
    double minimum{0.0};
    double maximum{0.0};
};

class IMetricsExporter {
public:
    virtual ~IMetricsExporter() = default;
    [[nodiscard]] virtual foundation::Result<void> exportObservation(
        const MetricObservation& observation) = 0;
};

class IMetricsService {
public:
    virtual ~IMetricsService() = default;
    [[nodiscard]] virtual foundation::Result<void> addCounter(
        kernel::MetricName name,
        double delta,
        MetricLabels labels = {}) = 0;
    [[nodiscard]] virtual foundation::Result<void> setGauge(
        kernel::MetricName name,
        double value,
        MetricLabels labels = {}) = 0;
    [[nodiscard]] virtual foundation::Result<void> observeHistogram(
        kernel::MetricName name,
        double value,
        MetricLabels labels = {}) = 0;
};

class LocalMetricsService final : public IMetricsService {
public:
    explicit LocalMetricsService(
        std::size_t seriesCapacity = 1024U,
        std::size_t failureCapacity = 256U);
    ~LocalMetricsService() override;

    LocalMetricsService(const LocalMetricsService&) = delete;
    LocalMetricsService& operator=(const LocalMetricsService&) = delete;

    [[nodiscard]] foundation::Result<void> addExporter(
        std::shared_ptr<IMetricsExporter> exporter);
    void freeze();
    [[nodiscard]] bool frozen() const;
    [[nodiscard]] foundation::Result<void> addCounter(
        kernel::MetricName name,
        double delta,
        MetricLabels labels = {}) override;
    [[nodiscard]] foundation::Result<void> setGauge(
        kernel::MetricName name,
        double value,
        MetricLabels labels = {}) override;
    [[nodiscard]] foundation::Result<void> observeHistogram(
        kernel::MetricName name,
        double value,
        MetricLabels labels = {}) override;
    [[nodiscard]] std::vector<MetricSnapshot> snapshot() const;
    [[nodiscard]] std::vector<foundation::Error> exporterFailures() const;

private:
    struct Impl;
    [[nodiscard]] foundation::Result<void> record(
        kernel::MetricName name,
        MetricKind kind,
        double value,
        MetricLabels labels);
    std::unique_ptr<Impl> impl_;
};

} // namespace lasercnc::observability
