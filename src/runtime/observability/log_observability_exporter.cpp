#include <lasercnc/observability/log_observability_exporter.hpp>

#include <lasercnc/foundation/error.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace lasercnc::observability {
namespace {

const char* traceStatusName(TraceStatus status) noexcept
{
    switch(status) {
    case TraceStatus::Running: return "running";
    case TraceStatus::Succeeded: return "succeeded";
    case TraceStatus::Failed: return "failed";
    case TraceStatus::Cancelled: return "cancelled";
    case TraceStatus::Stale: return "stale";
    }
    return "unknown";
}

LogLevel traceLogLevel(TraceStatus status) noexcept
{
    switch(status) {
    case TraceStatus::Succeeded: return LogLevel::Info;
    case TraceStatus::Cancelled:
    case TraceStatus::Stale: return LogLevel::Warning;
    case TraceStatus::Running:
    case TraceStatus::Failed: return LogLevel::Error;
    }
    return LogLevel::Error;
}

const char* metricKindName(MetricKind kind) noexcept
{
    switch(kind) {
    case MetricKind::Counter: return "counter";
    case MetricKind::Gauge: return "gauge";
    case MetricKind::Histogram: return "histogram";
    }
    return "unknown";
}

foundation::Value labelsValue(const MetricLabels& labels)
{
    foundation::Value::Object result;
    for(const auto& [name, value] : labels) {
        result.emplace(name, foundation::Value {value});
    }
    return foundation::Value {std::move(result)};
}

} // namespace

foundation::Result<std::shared_ptr<LogObservabilityExporter>>
LogObservabilityExporter::create(std::shared_ptr<ILogService> logService)
{
    if(logService == nullptr) {
        return foundation::Result<std::shared_ptr<LogObservabilityExporter>>::failure(
            foundation::makeError(
                "Observability.InvalidLogService",
                foundation::ErrorCategory::Validation,
                "A logging service is required by the observability exporter"));
    }
    return foundation::Result<std::shared_ptr<LogObservabilityExporter>>::success(
        std::shared_ptr<LogObservabilityExporter>(
            new LogObservabilityExporter(std::move(logService))));
}

LogObservabilityExporter::LogObservabilityExporter(
    std::shared_ptr<ILogService> logService)
    : logService_(std::move(logService))
{
}

foundation::Result<void> LogObservabilityExporter::exportSpan(
    const TraceSpanRecord& span)
{
    foundation::Value::Object data {
        {"attributes", foundation::Value {span.attributes}},
        {"durationMs",
         foundation::Value {std::chrono::duration<double, std::milli>(
             span.finishedAt - span.startedAt).count()}},
        {"name", foundation::Value {span.name}},
        {"spanId", foundation::Value {std::string(span.spanId.value())}},
        {"status", foundation::Value {traceStatusName(span.status)}},
    };
    if(span.parentSpanId.has_value()) {
        data.emplace(
            "parentSpanId",
            foundation::Value {std::string(span.parentSpanId->value())});
    }
    if(span.error.has_value()) {
        data.emplace(
            "errorCode",
            foundation::Value {std::string(span.error->code.value())});
        data.emplace("errorMessage", foundation::Value {span.error->message});
    }
    return logService_->write(LogRecord {
        span.finishedAt,
        traceLogLevel(span.status),
        "kernel.observability",
        "trace.span",
        "Trace span completed",
        LogContext {
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::string(span.traceId.value())},
        std::move(data)});
}

foundation::Result<void> LogObservabilityExporter::exportObservation(
    const MetricObservation& observation)
{
    return logService_->write(LogRecord {
        observation.timestamp,
        LogLevel::Info,
        "kernel.observability",
        "metric.observation",
        "Metric observation recorded",
        {},
        foundation::Value::Object {
            {"kind", foundation::Value {metricKindName(observation.kind)}},
            {"labels", labelsValue(observation.labels)},
            {"name", foundation::Value {std::string(observation.name.value())}},
            {"value", foundation::Value {observation.value}},
        }});
}

} // namespace lasercnc::observability
