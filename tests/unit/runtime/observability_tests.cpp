#include <lasercnc/observability/diagnostics_service.hpp>
#include <lasercnc/observability/log_observability_exporter.hpp>
#include <lasercnc/observability/metrics_service.hpp>
#include <lasercnc/observability/trace_service.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace lasercnc::foundation;
using namespace lasercnc::kernel;
using namespace lasercnc::observability;

namespace {

template <typename Id>
Id validId(const char* value)
{
    auto created = Id::create(value);
    if(!created) {
        throw std::logic_error("Invalid test ID");
    }
    return std::move(created).value();
}

class ReentrantFailingTraceExporter final : public ITraceExporter {
public:
    explicit ReentrantFailingTraceExporter(LocalTraceService& service) : service_(service) {}

    Result<void> exportSpan(const TraceSpanRecord&) override
    {
        observedRecords.store(service_.records().size());
        return Result<void>::failure(makeError(
            "Test.TraceExportFailed", ErrorCategory::Infrastructure, "expected"));
    }

    std::atomic_size_t observedRecords{0U};

private:
    LocalTraceService& service_;
};

class ReentrantFailingMetricsExporter final : public IMetricsExporter {
public:
    explicit ReentrantFailingMetricsExporter(LocalMetricsService& service) : service_(service) {}

    Result<void> exportObservation(const MetricObservation&) override
    {
        observedMetrics.store(service_.snapshot().size());
        return Result<void>::failure(makeError(
            "Test.MetricsExportFailed", ErrorCategory::Infrastructure, "expected"));
    }

    std::atomic_size_t observedMetrics{0U};

private:
    LocalMetricsService& service_;
};

class LambdaCheck final : public IDiagnosticCheck {
public:
    using Function = std::function<Result<DiagnosticReport>()>;
    explicit LambdaCheck(Function function) : function_(std::move(function)) {}
    Result<DiagnosticReport> run() override { return function_(); }

private:
    Function function_;
};

class RecordingLogService final : public ILogService {
public:
    Result<void> write(const LogRecord& record) override
    {
        std::lock_guard lock(mutex_);
        records_.push_back(record);
        return Result<void>::success();
    }

    Result<void> flush() override { return Result<void>::success(); }

    std::vector<LogRecord> records() const
    {
        std::lock_guard lock(mutex_);
        return records_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<LogRecord> records_;
};

} // namespace

TEST_CASE("LocalTraceService owns span completion and isolates exporters", "[observability][trace]")
{
    LocalTraceService traces;
    auto exporter = std::make_shared<ReentrantFailingTraceExporter>(traces);
    REQUIRE(traces.addExporter(exporter).hasValue());
    traces.freeze();
    CHECK(traces.frozen());
    CHECK_FALSE(traces.addExporter(exporter).hasValue());

    auto started = traces.startSpan(TraceSpanStart {
        validId<TraceId>("trace.local"),
        validId<SpanId>("span.root"),
        std::nullopt,
        "command.execute",
        Value::Object {{"command", Value {"kernel.test"}}}});
    REQUIRE(started.hasValue());
    CHECK(traces.activeSpanCount() == 1U);
    started.value()->end(TraceStatus::Succeeded);
    CHECK(traces.activeSpanCount() == 0U);
    REQUIRE(traces.records().size() == 1U);
    CHECK(traces.records().front().status == TraceStatus::Succeeded);
    CHECK(exporter->observedRecords.load() == 1U);
    REQUIRE(traces.exporterFailures().size() == 1U);
    CHECK(std::string(traces.exporterFailures().front().code.value())
          == "Test.TraceExportFailed");

    auto duplicate = traces.startSpan(TraceSpanStart {
        validId<TraceId>("trace.local"),
        validId<SpanId>("span.root"),
        std::nullopt,
        "duplicate",
        {}});
    REQUIRE_FALSE(duplicate.hasValue());
    CHECK(std::string(duplicate.error().code.value()) == "Trace.SpanIdAlreadyExists");

    {
        auto abandoned = traces.startSpan(TraceSpanStart {
            validId<TraceId>("trace.local"),
            validId<SpanId>("span.abandoned"),
            validId<SpanId>("span.root"),
            "task.execute",
            {}});
        REQUIRE(abandoned.hasValue());
    }
    REQUIRE(traces.records().size() == 2U);
    CHECK(traces.records()[1].status == TraceStatus::Failed);
    REQUIRE(traces.records()[1].error.has_value());
    CHECK(std::string(traces.records()[1].error->code.value()) == "Trace.SpanAbandoned");

    LocalTraceService bounded(1U, 1U);
    for(const auto* id : {"span.bounded.1", "span.bounded.2"}) {
        auto span = bounded.startSpan(TraceSpanStart {
            validId<TraceId>("trace.bounded"), validId<SpanId>(id), std::nullopt, "bounded", {}});
        REQUIRE(span.hasValue());
        span.value()->end(TraceStatus::Succeeded);
    }
    REQUIRE(bounded.records().size() == 1U);
    CHECK(bounded.records().front().spanId == validId<SpanId>("span.bounded.2"));
}

TEST_CASE("LocalMetricsService aggregates deterministically without exporter control flow", "[observability][metrics]")
{
    LocalMetricsService metrics;
    auto exporter = std::make_shared<ReentrantFailingMetricsExporter>(metrics);
    REQUIRE(metrics.addExporter(exporter).hasValue());
    const auto counter = validId<MetricName>("command.completed");
    const MetricLabels labels {{"outcome", "success"}};
    REQUIRE(metrics.addCounter(counter, 1.0, labels).hasValue());
    REQUIRE(metrics.addCounter(counter, 2.0, labels).hasValue());
    REQUIRE(metrics.setGauge(validId<MetricName>("task.active"), 3.0).hasValue());
    REQUIRE(metrics.observeHistogram(validId<MetricName>("task.duration_ms"), 4.0).hasValue());
    REQUIRE(metrics.observeHistogram(validId<MetricName>("task.duration_ms"), 10.0).hasValue());

    const auto snapshot = metrics.snapshot();
    REQUIRE(snapshot.size() == 3U);
    const auto& counterSnapshot = snapshot.front();
    CHECK(counterSnapshot.kind == MetricKind::Counter);
    CHECK(counterSnapshot.count == 2U);
    CHECK(counterSnapshot.value == 3.0);
    const auto& histogram = snapshot.back();
    CHECK(histogram.kind == MetricKind::Histogram);
    CHECK(histogram.count == 2U);
    CHECK(histogram.sum == 14.0);
    CHECK(histogram.minimum == 4.0);
    CHECK(histogram.maximum == 10.0);
    CHECK(exporter->observedMetrics.load() >= 1U);
    CHECK(metrics.exporterFailures().size() == 5U);

    auto negative = metrics.addCounter(counter, -1.0, labels);
    REQUIRE_FALSE(negative.hasValue());
    CHECK(std::string(negative.error().code.value()) == "Metrics.InvalidValue");
    CHECK_FALSE(metrics.setGauge(
        validId<MetricName>("invalid.nan"), std::nan("")).hasValue());
    auto conflicting = metrics.setGauge(counter, 1.0, labels);
    REQUIRE_FALSE(conflicting.hasValue());
    CHECK(std::string(conflicting.error().code.value()) == "Metrics.KindConflict");
    metrics.freeze();
    CHECK_FALSE(metrics.addExporter(exporter).hasValue());

    LocalMetricsService bounded(1U, 1U);
    REQUIRE(bounded.addCounter(validId<MetricName>("metric.first"), 1.0).hasValue());
    auto exhausted = bounded.addCounter(validId<MetricName>("metric.second"), 1.0);
    REQUIRE_FALSE(exhausted.hasValue());
    CHECK(std::string(exhausted.error().code.value()) == "Metrics.SeriesCapacityExceeded");
}

TEST_CASE("DiagnosticsService runs checks outside locks and converts failures to reports", "[observability][diagnostics]")
{
    DiagnosticsService diagnostics;
    const auto healthyId = validId<DiagnosticId>("kernel.healthy");
    const auto failingId = validId<DiagnosticId>("kernel.failing");
    const auto throwingId = validId<DiagnosticId>("kernel.throwing");
    auto healthy = std::make_shared<LambdaCheck>([&diagnostics, healthyId]() {
        static_cast<void>(diagnostics.latest());
        return Result<DiagnosticReport>::success(DiagnosticReport {
            healthyId,
            DiagnosticStatus::Healthy,
            "ready",
            Value {Value::Object {}},
            {}});
    });
    REQUIRE(diagnostics.registerCheck(healthyId, healthy).hasValue());
    REQUIRE(diagnostics
                .registerCheck(
                    failingId,
                    std::make_shared<LambdaCheck>([]() {
                        return Result<DiagnosticReport>::failure(makeError(
                            "Test.CheckFailed", ErrorCategory::Infrastructure, "expected"));
                    }))
                .hasValue());
    REQUIRE(diagnostics
                .registerCheck(
                    throwingId,
                    std::make_shared<LambdaCheck>([]() -> Result<DiagnosticReport> {
                        throw std::runtime_error("expected");
                    }))
                .hasValue());
    CHECK_FALSE(diagnostics.registerCheck(healthyId, healthy).hasValue());
    diagnostics.freeze();
    CHECK(diagnostics.frozen());
    CHECK_FALSE(diagnostics.registerCheck(
        validId<DiagnosticId>("kernel.late"), healthy).hasValue());

    const auto reports = diagnostics.runAll();
    REQUIRE(reports.size() == 3U);
    CHECK(reports[0].id == failingId);
    CHECK(reports[0].status == DiagnosticStatus::Unhealthy);
    CHECK(reports[1].id == healthyId);
    CHECK(reports[1].status == DiagnosticStatus::Healthy);
    CHECK(reports[2].id == throwingId);
    CHECK(reports[2].status == DiagnosticStatus::Unhealthy);
    CHECK(diagnostics.latest().size() == 3U);
    auto missing = diagnostics.run(validId<DiagnosticId>("kernel.missing"));
    REQUIRE_FALSE(missing.hasValue());
    CHECK(std::string(missing.error().code.value()) == "Diagnostics.NotFound");
}

TEST_CASE("LogObservabilityExporter maps spans and metrics to structured log records", "[observability][log]")
{
    CHECK_FALSE(LogObservabilityExporter::create(nullptr).hasValue());
    auto log = std::make_shared<RecordingLogService>();
    auto created = LogObservabilityExporter::create(log);
    REQUIRE(created.hasValue());
    const auto exporter = std::move(created).value();

    LocalTraceService traces;
    LocalMetricsService metrics;
    REQUIRE(traces.addExporter(exporter).hasValue());
    REQUIRE(metrics.addExporter(exporter).hasValue());
    auto span = traces.startSpan(TraceSpanStart {
        validId<TraceId>("trace.log-export"),
        validId<SpanId>("span.log-export"),
        validId<SpanId>("span.log-parent"),
        "command.execute",
        Value::Object {{"command", Value {"kernel.test"}}}});
    REQUIRE(span.hasValue());
    span.value()->end(TraceStatus::Cancelled, makeError(
        "Test.Cancelled", ErrorCategory::Cancellation, "expected"));
    REQUIRE(metrics.addCounter(
        validId<MetricName>("kernel.command.completed"),
        1.0,
        MetricLabels {{"outcome", "success"}}).hasValue());

    const auto records = log->records();
    REQUIRE(records.size() == 2U);
    CHECK(records[0].category == "trace.span");
    CHECK(records[0].level == LogLevel::Warning);
    REQUIRE(records[0].context.traceId.has_value());
    CHECK(*records[0].context.traceId == "trace.log-export");
    CHECK(records[0].structuredData.contains("parentSpanId"));
    CHECK(records[0].structuredData.contains("errorCode"));
    CHECK(records[1].category == "metric.observation");
    CHECK(records[1].structuredData.contains("labels"));
}
