#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/observability/log_service.hpp>
#include <lasercnc/observability/metrics_service.hpp>
#include <lasercnc/observability/trace_service.hpp>

#include <memory>

namespace lasercnc::observability {

class LogObservabilityExporter final : public ITraceExporter, public IMetricsExporter {
public:
    [[nodiscard]] static foundation::Result<std::shared_ptr<LogObservabilityExporter>> create(
        std::shared_ptr<ILogService> logService);

    [[nodiscard]] foundation::Result<void> exportSpan(
        const TraceSpanRecord& span) override;
    [[nodiscard]] foundation::Result<void> exportObservation(
        const MetricObservation& observation) override;

private:
    explicit LogObservabilityExporter(std::shared_ptr<ILogService> logService);

    std::shared_ptr<ILogService> logService_;
};

} // namespace lasercnc::observability
