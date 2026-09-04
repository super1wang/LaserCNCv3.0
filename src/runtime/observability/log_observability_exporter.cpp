#include <lasercnc/observability/log_observability_exporter.hpp>

#include <lasercnc/foundation/error.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <string>
#include <type_traits>
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

foundation::Result<void> invalidExport(const char* code, const char* message)
{
    return foundation::Result<void>::failure(foundation::makeError(
        code, foundation::ErrorCategory::Validation, message));
}

foundation::Result<void> exportFailure(const char* reason)
{
    return foundation::Result<void>::failure(foundation::makeError(
        "Observability.LogExportFailed", foundation::ErrorCategory::Internal,
        "The log observation could not be exported",
        foundation::Value{foundation::Value::Object{{"reason", foundation::Value{reason}}}}));
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
try {
    switch(span.status) {
    case TraceStatus::Succeeded:
    case TraceStatus::Failed:
    case TraceStatus::Cancelled:
    case TraceStatus::Stale:
        break;
    default:
        return invalidExport("Observability.InvalidSpanStatus", "Only completed spans can be exported");
    }
    if(span.name.empty()) {
        return invalidExport("Observability.InvalidSpanName", "An exported span name is required");
    }
    if(span.finishedAt < span.startedAt) {
        return invalidExport("Observability.InvalidSpanTimeRange", "Span completion cannot precede its start");
    }
    using ClockDuration = std::chrono::system_clock::duration;
    static_assert(std::is_integral_v<ClockDuration::rep> && std::is_signed_v<ClockDuration::rep>);
    using UnsignedTicks = std::make_unsigned_t<ClockDuration::rep>;
    // Ordered unsigned subtraction spans the entire signed clock range without wrapping signed arithmetic.
    // 中文翻译：先核对时间顺序，再用无符号差计算完整有符号时钟范围，保留极端位置的单 tick 差。
    const auto ticks = static_cast<UnsignedTicks>(span.finishedAt.time_since_epoch().count())
        - static_cast<UnsignedTicks>(span.startedAt.time_since_epoch().count());
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::duration<long double, ClockDuration::period>{static_cast<long double>(ticks)}).count();
    foundation::Value::Object data {
        {"attributes", foundation::Value {span.attributes}},
        {"durationMs",
         foundation::Value {elapsed}},
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
} catch(const std::exception& exception) {
    return exportFailure(exception.what());
} catch(...) {
    return exportFailure("Unknown failure");
}

foundation::Result<void> LogObservabilityExporter::exportObservation(
    const MetricObservation& observation)
try {
    switch(observation.kind) {
    case MetricKind::Counter:
    case MetricKind::Gauge:
    case MetricKind::Histogram:
        break;
    default:
        return invalidExport("Observability.InvalidMetricKind", "A declared metric kind is required");
    }
    if(!std::isfinite(observation.value)
       || (observation.kind == MetricKind::Counter && observation.value < 0.0)) {
        return invalidExport("Observability.InvalidMetricValue", "Metric values must be finite and counter deltas nonnegative");
    }
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
} catch(const std::exception& exception) {
    return exportFailure(exception.what());
} catch(...) {
    return exportFailure("Unknown failure");
}

} // namespace lasercnc::observability
