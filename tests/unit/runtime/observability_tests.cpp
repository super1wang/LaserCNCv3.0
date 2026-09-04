#include <lasercnc/observability/diagnostics_service.hpp>
#include <lasercnc/observability/log_observability_exporter.hpp>
#include <lasercnc/observability/metrics_service.hpp>
#include <lasercnc/observability/trace_service.hpp>

// Exercise the production arithmetic at an unreachable-in-practice uint64 counter boundary.
// 中文翻译：直接覆盖生产算术的 uint64 极限，不伪称实际调用了 2^64 次公共入口。
#include "../../../src/runtime/observability/metric_accumulation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
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

class ReentrantFailingDiagnosticExporter final : public IDiagnosticExporter {
public:
    explicit ReentrantFailingDiagnosticExporter(DiagnosticsService& service)
        : service_(service)
    {
    }

    Result<void> exportReport(const DiagnosticReport&) override
    {
        observedReports.store(service_.latest().size());
        return Result<void>::failure(makeError(
            "Test.DiagnosticExportFailed",
            ErrorCategory::Infrastructure,
            "expected"));
    }

    std::atomic_size_t observedReports{0U};

private:
    DiagnosticsService& service_;
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

class RecordingObserver final : public ITraceExporter, public IMetricsExporter, public IDiagnosticExporter {
public:
    Result<void> exportSpan(const TraceSpanRecord& record) override { spans.push_back(record); return Result<void>::success(); }
    Result<void> exportObservation(const MetricObservation& record) override { metrics.push_back(record); return Result<void>::success(); }
    Result<void> exportReport(const DiagnosticReport& record) override { diagnostics.push_back(record); return Result<void>::success(); }
    std::vector<TraceSpanRecord> spans;
    std::vector<MetricObservation> metrics;
    std::vector<DiagnosticReport> diagnostics;
};

class DiagnosticReentryDeadline final {
public:
    DiagnosticReentryDeadline() : worker_([ready = finished_.get_future()] {
        if(ready.wait_for(std::chrono::seconds{10}) != std::future_status::ready) {
            std::fputs("diagnostic-reentry-deadline-exceeded\n", stderr);
            std::fflush(stderr);
            std::_Exit(86);
        }
    }) {}
    ~DiagnosticReentryDeadline() { finished_.set_value(); }
private:
    std::promise<void> finished_;
    std::jthread worker_;
};

class DestructionCheck final : public IDiagnosticCheck {
public:
    explicit DestructionCheck(std::function<void()> action) : action_(std::move(action)) {}
    ~DestructionCheck() override { action_(); }
    Result<DiagnosticReport> run() override { throw std::logic_error("Rejected check must not run"); }
private:
    std::function<void()> action_;
};

} // namespace

TEST_CASE("LogObservabilityExporter rejects invalid direct metric values and kinds", "[observability][c6b7]")
{
    auto log = std::make_shared<RecordingLogService>();
    auto exporter = LogObservabilityExporter::create(log);
    REQUIRE(exporter);
    MetricObservation value{validId<MetricName>("metric.direct"), MetricKind::Counter, 0.0, {}, {}};
    for(unsigned int raw = 3U; raw <= 255U; ++raw) {
        value.kind = static_cast<MetricKind>(raw);
        const auto result = exporter.value()->exportObservation(value);
        CHECK_FALSE(result);
        if(!result) { CHECK(std::string(result.error().code.value()) == "Observability.InvalidMetricKind"); }
    }
    for(const auto kind : {MetricKind::Counter, MetricKind::Gauge, MetricKind::Histogram}) {
        value.kind = kind;
        for(const auto invalid : {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            value.value = invalid;
            CHECK_FALSE(exporter.value()->exportObservation(value));
        }
    }
    value.kind = MetricKind::Counter;
    value.value = -1.0;
    const auto negative = exporter.value()->exportObservation(value);
    CHECK_FALSE(negative);
    if(!negative) { CHECK(std::string(negative.error().code.value()) == "Observability.InvalidMetricValue"); }
    CHECK(log->records().empty());
}

TEST_CASE("LogObservabilityExporter rejects invalid direct span completion", "[observability][c6b7]")
{
    auto log = std::make_shared<RecordingLogService>();
    auto exporter = LogObservabilityExporter::create(log);
    REQUIRE(exporter);
    TraceSpanRecord span{validId<TraceId>("trace.direct"), validId<SpanId>("span.direct"), {}, "direct", {}, {}, TraceStatus::Succeeded, {}, {}};
    for(unsigned int raw = 0U; raw <= 255U; ++raw) {
        if(raw >= 1U && raw <= 4U) { continue; }
        span.status = static_cast<TraceStatus>(raw);
        const auto result = exporter.value()->exportSpan(span);
        CHECK_FALSE(result);
        if(!result) { CHECK(std::string(result.error().code.value()) == "Observability.InvalidSpanStatus"); }
    }
    span.status = TraceStatus::Succeeded;
    span.name.clear();
    CHECK_FALSE(exporter.value()->exportSpan(span));
    span.name = "direct";
    span.startedAt = span.finishedAt + std::chrono::system_clock::duration{1};
    const auto reversed = exporter.value()->exportSpan(span);
    CHECK_FALSE(reversed);
    if(!reversed) { CHECK(std::string(reversed.error().code.value()) == "Observability.InvalidSpanTimeRange"); }
    CHECK(log->records().empty());
}

TEST_CASE("LogObservabilityExporter calculates extreme elapsed times without signed wrap", "[observability][c6b7]")
{
    using Clock = std::chrono::system_clock;
    using Duration = Clock::duration;
    auto log = std::make_shared<RecordingLogService>();
    auto exporter = LogObservabilityExporter::create(log);
    REQUIRE(exporter);
    TraceSpanRecord span{validId<TraceId>("trace.extreme"), validId<SpanId>("span.extreme"), {}, "extreme", Clock::time_point::min(), Clock::time_point::max(), TraceStatus::Succeeded, {}, {}};
    REQUIRE(exporter.value()->exportSpan(span));
    const auto expected = static_cast<double>((static_cast<long double>(Duration::max().count()) - static_cast<long double>(Duration::min().count())) * Duration::period::num * 1000.0L / Duration::period::den);
    auto records = log->records();
    const auto actual = *records.back().structuredData.at("durationMs").getIf<double>();
    CHECK(std::isfinite(actual));
    CHECK(actual > 0.0);
    CHECK(std::abs(actual - expected) <= expected * 1e-14);
    span.startedAt = Clock::time_point::max() - Duration{1};
    REQUIRE(exporter.value()->exportSpan(span));
    records = log->records();
    CHECK(*records.back().structuredData.at("durationMs").getIf<double>() == std::chrono::duration<double, std::milli>(Duration{1}).count());
}

TEST_CASE("LogObservabilityExporter contains direct log backend exceptions", "[observability][c6b7]")
{
    class ThrowingLog final : public ILogService {
    public:
        bool unknown{false};
        Result<void> write(const LogRecord&) override {
            if(unknown) { throw 42; }
            throw std::runtime_error("log backend threw");
        }
        Result<void> flush() override { return Result<void>::success(); }
    };
    for(const bool unknown : {false, true}) {
        auto log = std::make_shared<ThrowingLog>();
        log->unknown = unknown;
        auto exporter = LogObservabilityExporter::create(log);
        REQUIRE(exporter);
        TraceSpanRecord span{validId<TraceId>("trace.throw"), validId<SpanId>("span.throw"), {}, "throw", {}, {}, TraceStatus::Succeeded, {}, {}};
        for(const bool metric : {false, true}) {
            DYNAMIC_SECTION("unknown=" << unknown << " metric=" << metric) {
                const auto result = metric ? exporter.value()->exportObservation({validId<MetricName>("metric.throw"), MetricKind::Gauge, 1.0, {}, {}}) : exporter.value()->exportSpan(span);
                REQUIRE_FALSE(result);
                CHECK(std::string(result.error().code.value()) == "Observability.LogExportFailed");
            }
        }
    }
}

TEST_CASE("LogObservabilityExporter preserves direct legal mappings and small elapsed times", "[observability][c6b7]")
{
    using Clock = std::chrono::system_clock;
    using Duration = Clock::duration;
    auto log = std::make_shared<RecordingLogService>();
    auto exporter = LogObservabilityExporter::create(log);
    REQUIRE(exporter);
    TraceSpanRecord span{validId<TraceId>("trace.valid-direct"), validId<SpanId>("span.valid-direct"), {}, "valid", {}, {}, TraceStatus::Succeeded, {}, {}};
    for(const auto status : {TraceStatus::Succeeded, TraceStatus::Failed, TraceStatus::Cancelled, TraceStatus::Stale}) {
        span.status = status;
        REQUIRE(exporter.value()->exportSpan(span));
        const auto record = log->records().back();
        CHECK(record.structuredData.at("durationMs") == Value{0.0});
        const auto expected = status == TraceStatus::Succeeded ? LogLevel::Info : status == TraceStatus::Failed ? LogLevel::Error : LogLevel::Warning;
        CHECK(record.level == expected);
    }
    for(const auto start : {Clock::time_point::min(), Clock::time_point{} - Duration{2}}) {
        span.startedAt = start;
        span.finishedAt = start + Duration{5};
        REQUIRE(exporter.value()->exportSpan(span));
        CHECK(log->records().back().structuredData.at("durationMs") == Value{std::chrono::duration<double, std::milli>(Duration{5}).count()});
    }
    for(const auto kind : {MetricKind::Counter, MetricKind::Gauge, MetricKind::Histogram}) {
        for(const double value : {0.0, -1.0, std::numeric_limits<double>::max()}) {
            if(kind == MetricKind::Counter && value < 0.0) { continue; }
            REQUIRE(exporter.value()->exportObservation({validId<MetricName>("metric.valid-direct"), kind, value, {}, {}}));
            CHECK(log->records().back().structuredData.at("value") == Value{value});
        }
    }
}

TEST_CASE("LogObservabilityExporter preserves returned backend errors without retry", "[observability][c6b7]")
{
    class FailedLog final : public ILogService {
    public:
        unsigned int calls{0U};
        Error error = makeError("Test.Backend", ErrorCategory::Infrastructure, "backend", Value{"detail"}, Severity::Warning,
            std::make_shared<const Error>(makeError("Test.Cause", ErrorCategory::Internal, "cause")));
        Result<void> write(const LogRecord&) override { ++calls; return Result<void>::failure(error); }
        Result<void> flush() override { FAIL_CHECK("Unexpected flush"); return Result<void>::success(); }
    };
    auto log = std::make_shared<FailedLog>();
    auto exporter = LogObservabilityExporter::create(log);
    REQUIRE(exporter);
    TraceSpanRecord span{validId<TraceId>("trace.backend"), validId<SpanId>("span.backend"), {}, "backend", {}, {}, TraceStatus::Succeeded, {}, {}};
    for(const bool metric : {false, true}) {
        const auto result = metric ? exporter.value()->exportObservation({validId<MetricName>("metric.backend"), MetricKind::Counter, 1.0, {}, {}}) : exporter.value()->exportSpan(span);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == log->error.code);
        CHECK(result.error().category == log->error.category);
        CHECK(result.error().severity == log->error.severity);
        CHECK(result.error().details == log->error.details);
        CHECK(result.error().cause == log->error.cause);
    }
    CHECK(log->calls == 2U);
}

TEST_CASE("LocalTraceService normalizes every invalid terminal status to failed", "[observability][c6b6]")
{
    for(unsigned int raw = 0U; raw <= 255U; ++raw) {
        if(raw >= 1U && raw <= 4U) { continue; }
        INFO("raw=" << raw);
        LocalTraceService traces;
        auto observer = std::make_shared<RecordingObserver>();
        REQUIRE(traces.addExporter(observer));
        auto span = traces.startSpan({validId<TraceId>("trace.invalid"), validId<SpanId>("span.invalid"), {}, "invalid", {}});
        REQUIRE(span);
        span.value()->end(static_cast<TraceStatus>(raw), makeError("Test.Original", ErrorCategory::Internal, "original"));
        CHECK(traces.activeSpanCount() == 0U);
        const auto records = traces.records();
        REQUIRE(records.size() == 1U);
        CHECK(records.front().status == TraceStatus::Failed);
        REQUIRE(records.front().error);
        CHECK(std::string(records.front().error->code.value()) == "Trace.InvalidTerminalStatus");
        if(records.front().error->cause) { CHECK(std::string(records.front().error->cause->code.value()) == "Test.Original"); }
        else { FAIL_CHECK("Original error cause was lost"); }
        REQUIRE(observer->spans.size() == 1U);
        CHECK(observer->spans.front().status == TraceStatus::Failed);
        span.value()->end(TraceStatus::Succeeded);
        span.value().reset();
        CHECK(traces.records().size() == 1U);
        CHECK(observer->spans.size() == 1U);
    }
}

TEST_CASE("LocalMetricsService rejects aggregate overflow without recording or exporting", "[observability][c6b6]")
{
    for(const auto kind : {MetricKind::Counter, MetricKind::Gauge, MetricKind::Histogram}) {
        for(const double sign : {1.0, -1.0}) {
            if(kind == MetricKind::Counter && sign < 0.0) { continue; }
            DYNAMIC_SECTION("kind=" << static_cast<unsigned int>(kind) << " sign=" << sign) {
                LocalMetricsService service;
                auto observer = std::make_shared<RecordingObserver>();
                REQUIRE(service.addExporter(observer));
                const auto name = validId<MetricName>("metric.overflow");
                const auto submit = [&](double value) {
                    if(kind == MetricKind::Counter) { return service.addCounter(name, value); }
                    if(kind == MetricKind::Gauge) { return service.setGauge(name, value); }
                    return service.observeHistogram(name, value);
                };
                const double extreme = sign * std::numeric_limits<double>::max();
                REQUIRE(submit(extreme));
                const auto rejected = submit(extreme);
                CHECK_FALSE(rejected);
                if(!rejected) { CHECK(std::string(rejected.error().code.value()) == "Metrics.AggregateOverflow"); }
                const auto snapshot = service.snapshot();
                REQUIRE(snapshot.size() == 1U);
                CHECK(snapshot[0].count == 1U);
                CHECK(snapshot[0].sum == extreme);
                CHECK(snapshot[0].value == extreme);
                CHECK(snapshot[0].minimum == extreme);
                CHECK(snapshot[0].maximum == extreme);
                CHECK(observer->metrics.size() == 1U);
                REQUIRE(submit(0.0));
                CHECK(service.snapshot()[0].count == 2U);
                CHECK(std::isfinite(service.snapshot()[0].sum));
                CHECK(observer->metrics.size() == 2U);
            }
        }
    }
}

TEST_CASE("DiagnosticsService converts every undefined status to unhealthy", "[observability][c6b6]")
{
    DiagnosticsService service;
    auto observer = std::make_shared<RecordingObserver>();
    REQUIRE(service.addExporter(observer));
    const auto id = validId<DiagnosticId>("diagnostic.invalid-status");
    unsigned int raw = 4U;
    REQUIRE(service.registerCheck(id, std::make_shared<LambdaCheck>([&] {
        return Result<DiagnosticReport>::success({id, static_cast<DiagnosticStatus>(raw), "invalid", {}, {}});
    })));
    for(; raw <= 255U; ++raw) {
        INFO("raw=" << raw);
        auto result = service.run(id);
        REQUIRE(result);
        CHECK(result.value().status == DiagnosticStatus::Unhealthy);
        CHECK(service.latest().front().status == DiagnosticStatus::Unhealthy);
        const auto* details = result.value().details.getIf<Value::Object>();
        REQUIRE(details);
        CHECK(details->at("errorCode") == Value{"Diagnostics.InvalidStatus"});
        CHECK(details->at("reportedStatus") == Value{static_cast<std::int64_t>(raw)});
        REQUIRE(observer->diagnostics.size() == raw - 3U);
        CHECK(observer->diagnostics.back().status == DiagnosticStatus::Unhealthy);
    }
}

TEST_CASE("DiagnosticsService releases rejected check ownership outside its lock", "[observability][c6b6]")
{
    DiagnosticReentryDeadline deadline;
    DiagnosticsService service;
    const auto id = validId<DiagnosticId>("diagnostic.duplicate");
    REQUIRE(service.registerCheck(id, std::make_shared<LambdaCheck>([&] {
        return Result<DiagnosticReport>::success({id, DiagnosticStatus::Healthy, "kept", {}, {}});
    })));
    REQUIRE(service.run(id));
    unsigned int destroyed = 0U;
    std::size_t observed = 0U;
    const auto rejected = service.registerCheck(id, std::make_shared<DestructionCheck>([&] {
        ++destroyed;
        observed = service.latest().size();
    }));
    REQUIRE_FALSE(rejected);
    CHECK(std::string(rejected.error().code.value()) == "Diagnostics.AlreadyRegistered");
    CHECK(destroyed == 1U);
    CHECK(observed == 1U);
    const auto kept = service.run(id);
    REQUIRE(kept);
    CHECK(kept.value().status == DiagnosticStatus::Healthy);
}

TEST_CASE("LocalTraceService preserves valid terminals and explicit invalid status diagnostics", "[observability][c6b6]")
{
    for(const auto status : {TraceStatus::Succeeded, TraceStatus::Failed, TraceStatus::Cancelled, TraceStatus::Stale}) {
        for(const bool withError : {false, true}) {
            LocalTraceService service;
            auto observer = std::make_shared<RecordingObserver>();
            REQUIRE(service.addExporter(observer));
            auto span = service.startSpan({validId<TraceId>("trace.valid"), validId<SpanId>("span.valid"), {}, "valid", {}});
            REQUIRE(span);
            std::optional<Error> error;
            if(withError) { error = makeError("Test.Provided", ErrorCategory::Internal, "provided"); }
            span.value()->end(status, error);
            const auto records = service.records();
            REQUIRE(records.size() == 1U);
            CHECK(records[0].status == status);
            CHECK(records[0].error.has_value() == withError);
            if(withError) { CHECK(std::string(records[0].error->code.value()) == "Test.Provided"); }
            CHECK(observer->spans.front().status == status);
            span.value()->end(static_cast<TraceStatus>(255U));
            span.value().reset();
            CHECK(observer->spans.size() == 1U);
        }
    }
    for(const auto raw : {0U, 255U}) {
        LocalTraceService service;
        auto span = service.startSpan({validId<TraceId>("trace.no-cause"), validId<SpanId>("span.no-cause"), {}, "invalid", {}});
        REQUIRE(span);
        span.value()->end(static_cast<TraceStatus>(raw));
        const auto records = service.records();
        REQUIRE(records.size() == 1U);
        REQUIRE(records[0].error);
        CHECK(records[0].error->category == ErrorCategory::Validation);
        CHECK_FALSE(records[0].error->cause);
        const auto* details = records[0].error->details.getIf<Value::Object>();
        REQUIRE(details);
        CHECK(details->at("requestedStatus") == Value{static_cast<std::int64_t>(raw)});
    }
}

TEST_CASE("LocalMetricsService production arithmetic prevents count wrap without mutation", "[observability][c6b6]")
{
    for(const auto kind : {MetricKind::Counter, MetricKind::Gauge, MetricKind::Histogram}) {
        lasercnc::observability::detail::MetricAggregate aggregate{kind, std::numeric_limits<std::uint64_t>::max() - 1U, 2.0, 2.0, 2.0, 2.0};
        REQUIRE(lasercnc::observability::detail::tryAccumulateMetric(aggregate, 1.0));
        CHECK(aggregate.count == std::numeric_limits<std::uint64_t>::max());
        const auto before = aggregate;
        for(const double value : {0.0, 1.0}) {
            CHECK_FALSE(lasercnc::observability::detail::tryAccumulateMetric(aggregate, value));
            CHECK(aggregate.count == before.count);
            CHECK(aggregate.sum == before.sum);
            CHECK(aggregate.value == before.value);
            CHECK(aggregate.minimum == before.minimum);
            CHECK(aggregate.maximum == before.maximum);
        }
    }
}

TEST_CASE("LocalMetricsService preserves signed finite extrema and cancellation", "[observability][c6b6]")
{
    for(const auto kind : {MetricKind::Gauge, MetricKind::Histogram}) {
        LocalMetricsService service;
        const auto name = validId<MetricName>("metric.signed");
        const auto submit = [&](double value) {
            return kind == MetricKind::Gauge ? service.setGauge(name, value) : service.observeHistogram(name, value);
        };
        const auto maximum = std::numeric_limits<double>::max();
        REQUIRE(submit(maximum));
        REQUIRE(submit(-maximum));
        REQUIRE(submit(-0.0));
        const auto snapshot = service.snapshot();
        REQUIRE(snapshot.size() == 1U);
        CHECK(snapshot[0].count == 3U);
        CHECK(snapshot[0].sum == 0.0);
        CHECK(snapshot[0].minimum == -maximum);
        CHECK(snapshot[0].maximum == maximum);
        CHECK(std::signbit(snapshot[0].value));
    }
}

TEST_CASE("DiagnosticsService preserves every declared status and frozen rejection ownership", "[observability][c6b6]")
{
    DiagnosticReentryDeadline deadline;
    DiagnosticsService service;
    auto observer = std::make_shared<RecordingObserver>();
    REQUIRE(service.addExporter(observer));
    const auto id = validId<DiagnosticId>("diagnostic.valid-status");
    auto status = DiagnosticStatus::Healthy;
    REQUIRE(service.registerCheck(id, std::make_shared<LambdaCheck>([&] {
        return Result<DiagnosticReport>::success({id, status, "valid", Value{"kept"}, {}});
    })));
    for(const auto declared : {DiagnosticStatus::Healthy, DiagnosticStatus::Degraded, DiagnosticStatus::Unhealthy, DiagnosticStatus::Unknown}) {
        status = declared;
        auto report = service.run(id);
        REQUIRE(report);
        CHECK(report.value().status == declared);
        CHECK(report.value().details == Value{"kept"});
        CHECK(observer->diagnostics.back().status == declared);
    }
    service.freeze();
    unsigned int destroyed = 0U;
    bool observedFrozen = false;
    const auto rejected = service.registerCheck(id, std::make_shared<DestructionCheck>([&] { ++destroyed; observedFrozen = service.frozen(); }));
    REQUIRE_FALSE(rejected);
    CHECK(std::string(rejected.error().code.value()) == "Diagnostics.RegistryFrozen");
    CHECK(destroyed == 1U);
    CHECK(observedFrozen);
}

TEST_CASE("LocalTraceService concurrent valid and invalid completions publish only one terminal", "[observability][c6b6]")
{
    for(unsigned int iteration = 0U; iteration < 8U; ++iteration) {
        LocalTraceService service;
        auto observer = std::make_shared<RecordingObserver>();
        REQUIRE(service.addExporter(observer));
        auto span = service.startSpan({validId<TraceId>("trace.concurrent"), validId<SpanId>("span.concurrent"), {}, "concurrent", {}});
        REQUIRE(span);
        std::promise<void> release;
        auto ready = release.get_future().share();
        std::jthread valid([&] { if(ready.wait_for(std::chrono::seconds{5}) == std::future_status::ready) { span.value()->end(TraceStatus::Succeeded); } });
        std::jthread invalid([&] { if(ready.wait_for(std::chrono::seconds{5}) == std::future_status::ready) { span.value()->end(static_cast<TraceStatus>(255U)); } });
        release.set_value();
        valid.join();
        invalid.join();
        CHECK(service.activeSpanCount() == 0U);
        const auto records = service.records();
        REQUIRE(records.size() == 1U);
        CHECK((records[0].status == TraceStatus::Succeeded || records[0].status == TraceStatus::Failed));
        if(records[0].status == TraceStatus::Failed) {
            REQUIRE(records[0].error);
            CHECK(std::string(records[0].error->code.value()) == "Trace.InvalidTerminalStatus");
        }
        REQUIRE(observer->spans.size() == 1U);
        CHECK(observer->spans[0].status == records[0].status);
    }
}

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
    DiagnosticsService diagnostics(1U);
    auto exporter = std::make_shared<ReentrantFailingDiagnosticExporter>(diagnostics);
    REQUIRE(diagnostics.addExporter(exporter).hasValue());
    CHECK_FALSE(diagnostics.addExporter(nullptr).hasValue());
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
    CHECK_FALSE(diagnostics.addExporter(exporter).hasValue());
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
    CHECK(exporter->observedReports.load() == 3U);
    REQUIRE(diagnostics.exporterFailures().size() == 1U);
    CHECK(std::string(diagnostics.exporterFailures().front().code.value())
          == "Test.DiagnosticExportFailed");
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
