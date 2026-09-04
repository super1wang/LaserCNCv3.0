#include <lasercnc/observability/diagnostics_service.hpp>
#include <lasercnc/observability/metrics_service.hpp>
#include <lasercnc/observability/trace_service.hpp>

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <malloc.h>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#ifdef _DEBUG
#include <crtdbg.h>
#endif

namespace allocation_probe {
thread_local bool armed = false;
thread_local std::size_t calls = 0;
thread_local std::size_t failAt = std::numeric_limits<std::size_t>::max();
thread_local bool injected = false;
thread_local bool persistent = false;

void beforeAllocation()
{
    if(!armed) { return; }
    const auto index = calls++;
    if(index == failAt || (persistent && index > failAt)) {
        injected = true;
        if(!persistent) { armed = false; }
        throw std::bad_alloc{};
    }
}

struct Scope final {
    explicit Scope(std::size_t index, bool remainFailing = false)
    {
        calls = 0;
        failAt = index;
        injected = false;
        persistent = remainFailing;
        armed = true;
    }
    ~Scope() { armed = false; }
};
}

// Exercise production allocations without adding fault switches to the Kernel.
// 中文翻译：替换仅作用于本进程，覆盖生产代码分配，不向内核增加故障开关。
void* operator new(std::size_t size)
{
    allocation_probe::beforeAllocation();
    if(void* memory = std::malloc(size == 0 ? 1 : size)) { return memory; }
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void* operator new(std::size_t size, std::align_val_t alignment)
{
    allocation_probe::beforeAllocation();
    if(void* memory = _aligned_malloc(size == 0 ? 1 : size, static_cast<std::size_t>(alignment))) {
        return memory;
    }
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void operator delete(void* memory, std::align_val_t) noexcept { _aligned_free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { _aligned_free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { _aligned_free(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { _aligned_free(memory); }

using namespace lasercnc::foundation;
using namespace lasercnc::kernel;
using namespace lasercnc::observability;

namespace {
template<class Id> Id id(const char* value)
{
    auto result = Id::create(value);
    if(!result) { throw std::logic_error("Invalid probe ID"); }
    return std::move(result).value();
}

bool require(bool condition, const char* message, std::size_t index)
{
    if(!condition) { std::fprintf(stderr, "trace-allocation-failed index=%zu: %s\n", index, message); }
    return condition;
}

struct CounterExporter final : ITraceExporter {
    Result<void> exportSpan(const TraceSpanRecord&) override
    {
        ++calls;
        return Result<void>::success();
    }
    std::size_t calls{0};
};

void armNextAllocation(bool remainFailing)
{
    allocation_probe::calls = 0;
    allocation_probe::failAt = 0;
    allocation_probe::injected = false;
    allocation_probe::persistent = remainFailing;
    allocation_probe::armed = true;
}

struct ArmingFailureExporter final : ITraceExporter, IMetricsExporter, IDiagnosticExporter {
    ArmingFailureExporter(bool throws, bool persistentFailure)
        : throws_(throws), persistentFailure_(persistentFailure),
          failure_(makeError("Probe.ExporterFailed", ErrorCategory::Infrastructure, "expected"))
    {
    }

    Result<void> exportSpan(const TraceSpanRecord&) override { return fail(); }
    Result<void> exportObservation(const MetricObservation&) override { return fail(); }
    Result<void> exportReport(const DiagnosticReport&) override { return fail(); }

private:
    Result<void> fail()
    {
        if(throws_) {
            armNextAllocation(persistentFailure_);
            throw 7;
        }
        auto failed = Result<void>::failure(failure_);
        armNextAllocation(persistentFailure_);
        return failed;
    }

    bool throws_;
    bool persistentFailure_;
    Error failure_;
};

struct FollowingExporter final : ITraceExporter, IMetricsExporter, IDiagnosticExporter {
    Result<void> exportSpan(const TraceSpanRecord&) override { ++traceCalls; return Result<void>::success(); }
    Result<void> exportObservation(const MetricObservation&) override { ++metricCalls; return Result<void>::success(); }
    Result<void> exportReport(const DiagnosticReport&) override { ++diagnosticCalls; return Result<void>::success(); }
    std::size_t traceCalls{0};
    std::size_t metricCalls{0};
    std::size_t diagnosticCalls{0};
};

struct HealthyCheck final : IDiagnosticCheck {
    explicit HealthyCheck(DiagnosticId id) : id_(std::move(id)) {}
    Result<DiagnosticReport> run() override
    {
        return Result<DiagnosticReport>::success({id_, DiagnosticStatus::Healthy, "healthy", Value{}, {}});
    }
    DiagnosticId id_;
};

struct AddingTraceExporter final : ITraceExporter {
    LocalTraceService* service{};
    std::shared_ptr<ITraceExporter> late;
    Result<void> exportSpan(const TraceSpanRecord&) override
    {
        ++calls;
        if(!added) { added = service->addExporter(late).hasValue(); }
        return Result<void>::success();
    }
    std::size_t calls{0};
    bool added{false};
};

struct AddingMetricsExporter final : IMetricsExporter {
    LocalMetricsService* service{};
    std::shared_ptr<IMetricsExporter> late;
    Result<void> exportObservation(const MetricObservation&) override
    {
        ++calls;
        if(!added) { added = service->addExporter(late).hasValue(); }
        return Result<void>::success();
    }
    std::size_t calls{0};
    bool added{false};
};

struct AddingDiagnosticExporter final : IDiagnosticExporter {
    DiagnosticsService* service{};
    std::shared_ptr<IDiagnosticExporter> late;
    Result<void> exportReport(const DiagnosticReport&) override
    {
        ++calls;
        if(!added) { added = service->addExporter(late).hasValue(); }
        return Result<void>::success();
    }
    std::size_t calls{0};
    bool added{false};
};

TraceSpanStart request()
{
    return {id<TraceId>("trace.allocation.long-identity-outside-small-string-storage"),
        id<SpanId>("span.allocation.long-identity-outside-small-string-storage"),
        id<SpanId>("span.allocation.parent-outside-small-string-storage"),
        std::string(180, 'n'), {{"payload", Value{std::string(512, 'p')}}}};
}

struct Fixture final {
    LocalTraceService service{2};
    std::shared_ptr<CounterExporter> exporter = std::make_shared<CounterExporter>();
    std::unique_ptr<ITraceSpan> other;
    Fixture()
    {
        if(!service.addExporter(exporter)) { throw std::logic_error("Exporter setup failed"); }
        auto completed = service.startSpan({id<TraceId>("trace.seed"), id<SpanId>("span.seed"), {}, "seed", {}});
        if(!completed) { throw std::logic_error("Seed setup failed"); }
        completed.value()->end(TraceStatus::Succeeded);
        auto active = service.startSpan({id<TraceId>("trace.other"), id<SpanId>("span.other"), {}, "other", {}});
        if(!active) { throw std::logic_error("Active setup failed"); }
        other = std::move(active).value();
    }
};

bool attempt(std::size_t failIndex, std::size_t& allocations, bool expectFailure)
{
    Fixture fixture;
    auto start = request();
    std::optional<Result<std::unique_ptr<ITraceSpan>>> result;
    bool threw = false;
    {
        allocation_probe::Scope scope{failIndex};
        try { result.emplace(fixture.service.startSpan(std::move(start))); }
        catch(const std::bad_alloc&) { threw = true; }
    }
    allocations = allocation_probe::calls;
    bool ok = require(allocation_probe::injected == expectFailure, "injection coverage", failIndex);
    ok &= require(threw == expectFailure, "bad_alloc propagation", failIndex);
    ok &= require(fixture.exporter->calls == 1, "admission must not export", failIndex);
    const auto records = fixture.service.records();
    ok &= require(records.size() == 1 && records.front().spanId == id<SpanId>("span.seed")
        && records.front().status == TraceStatus::Succeeded && records.front().name == "seed",
        "retained record unchanged", failIndex);
    ok &= require(fixture.service.activeSpanCount() == (expectFailure ? 1U : 2U),
        "active registry atomicity", failIndex);
    if(expectFailure) {
        ok &= require(!result.has_value(), "no result on allocation exception", failIndex);
        auto retry = fixture.service.startSpan(request());
        ok &= require(retry.hasValue(), "same identity must remain retryable", failIndex);
        if(retry) { retry.value()->end(TraceStatus::Succeeded); }
    } else {
        ok &= require(result && result->hasValue(), "successful result owns handle", failIndex);
        if(result && *result) { result->value()->end(TraceStatus::Succeeded); }
    }
    fixture.other->end(TraceStatus::Cancelled);
    ok &= require(fixture.service.activeSpanCount() == 0, "no orphan after cleanup", failIndex);
    ok &= require(fixture.exporter->calls == 3, "only real completions exported", failIndex);
    ok &= require(fixture.service.exporterFailures().empty(), "no synthetic exporter failure", failIndex);
    return ok;
}

bool completionAttempt(
    std::size_t failIndex,
    std::size_t& allocations,
    bool expectFailure,
    TraceStatus requestedStatus,
    bool persistentFailure = false,
    bool abandon = false)
{
    Fixture fixture;
    auto started = fixture.service.startSpan(request());
    if(!started) { throw std::logic_error("Completion target setup failed"); }
    auto target = std::move(started).value();
    {
        allocation_probe::Scope scope{failIndex, persistentFailure};
        if(abandon) { target.reset(); }
        else { target->end(requestedStatus); }
    }
    allocations = allocation_probe::calls;
    bool ok = require(allocation_probe::injected == expectFailure, "completion injection coverage", failIndex);
    ok &= require(fixture.service.activeSpanCount() == 1U,
        "completed handle must never leave an active orphan", failIndex);
    target.reset();
    ok &= require(fixture.service.activeSpanCount() == 1U,
        "completed handle destruction must remain idempotent", failIndex);

    const auto records = fixture.service.records();
    const auto targetId = id<SpanId>("span.allocation.long-identity-outside-small-string-storage");
    bool targetRetained = false;
    for(const auto& record : records) {
        if(record.spanId == targetId) { targetRetained = true; }
    }
    ok &= require(fixture.exporter->calls == (targetRetained ? 2U : 1U),
        "retained completion must reach exporter snapshot", failIndex);
    auto retry = fixture.service.startSpan(request());
    ok &= require(retry.hasValue() != targetRetained,
        "identity retry must agree with bounded retained history", failIndex);
    if(retry) { retry.value()->end(TraceStatus::Cancelled); }
    fixture.other->end(TraceStatus::Cancelled);
    ok &= require(fixture.service.activeSpanCount() == 0U, "completion cleanup has no orphan", failIndex);
    return ok;
}

bool verifyCompletionPath(TraceStatus status, std::size_t& count, bool abandon = false)
{
    if(!completionAttempt(std::numeric_limits<std::size_t>::max(), count, false, status, false, abandon)
       || count == 0 || count > 1000) {
        return false;
    }
    bool ok = true;
    for(std::size_t index = 0; index < count; ++index) {
        std::size_t observed = 0;
        ok &= completionAttempt(index, observed, true, status, false, abandon);
        ok &= require(observed == index + 1, "exact completion allocation index reached", index);
        observed = 0;
        ok &= completionAttempt(index, observed, true, status, true, abandon);
        ok &= require(observed >= index + 1, "persistent completion failure reached", index);
    }
    std::size_t observed = 0;
    ok &= completionAttempt(count, observed, false, status, false, abandon);
    ok &= require(observed == count, "stable completion allocation path", count);
    return ok;
}

bool verifyExporterBookkeeping(bool throws, bool persistentFailure, std::size_t scenario)
{
    bool ok = true;
    {
        LocalTraceService service;
        auto failing = std::make_shared<ArmingFailureExporter>(throws, persistentFailure);
        auto following = std::make_shared<FollowingExporter>();
        if(!service.addExporter(failing) || !service.addExporter(following)) {
            throw std::logic_error("Trace exporter setup failed");
        }
        auto started = service.startSpan({id<TraceId>("trace.exporter-bookkeeping"),
            id<SpanId>("span.exporter-bookkeeping"), {}, "exporter-bookkeeping", {}});
        if(!started) { throw std::logic_error("Trace setup failed"); }
        started.value()->end(TraceStatus::Succeeded);
        allocation_probe::armed = false;
        ok &= require(allocation_probe::injected, "trace bookkeeping injection", scenario);
        ok &= require(following->traceCalls == 1U, "trace later exporter called", scenario);
        ok &= require(service.activeSpanCount() == 0U && service.records().size() == 1U,
            "trace local fact preserved", scenario);
    }
    {
        LocalMetricsService service;
        auto failing = std::make_shared<ArmingFailureExporter>(throws, persistentFailure);
        auto following = std::make_shared<FollowingExporter>();
        if(!service.addExporter(failing) || !service.addExporter(following)) {
            throw std::logic_error("Metrics exporter setup failed");
        }
        std::optional<Result<void>> recorded;
        bool escaped = false;
        try { recorded.emplace(service.addCounter(id<MetricName>("metric.exporter-bookkeeping"), 1.0)); }
        catch(...) { escaped = true; }
        allocation_probe::armed = false;
        ok &= require(allocation_probe::injected, "metrics bookkeeping injection", scenario);
        ok &= require(!escaped && recorded && recorded->hasValue(), "metrics result preserved", scenario);
        ok &= require(following->metricCalls == 1U, "metrics later exporter called", scenario);
        const auto snapshot = service.snapshot();
        ok &= require(snapshot.size() == 1U && snapshot.front().value == 1.0,
            "metrics local fact preserved", scenario);
    }
    {
        DiagnosticsService service;
        const auto diagnosticId = id<DiagnosticId>("diagnostic.exporter-bookkeeping");
        auto failing = std::make_shared<ArmingFailureExporter>(throws, persistentFailure);
        auto following = std::make_shared<FollowingExporter>();
        if(!service.registerCheck(diagnosticId, std::make_shared<HealthyCheck>(diagnosticId))
           || !service.addExporter(failing) || !service.addExporter(following)) {
            throw std::logic_error("Diagnostics exporter setup failed");
        }
        std::optional<Result<DiagnosticReport>> reported;
        bool escaped = false;
        try { reported.emplace(service.run(diagnosticId)); }
        catch(...) { escaped = true; }
        allocation_probe::armed = false;
        ok &= require(allocation_probe::injected, "diagnostics bookkeeping injection", scenario);
        ok &= require(!escaped && reported && reported->hasValue(), "diagnostics result preserved", scenario);
        ok &= require(following->diagnosticCalls == 1U, "diagnostics later exporter called", scenario);
        ok &= require(service.latest().size() == 1U, "diagnostics local fact preserved", scenario);
    }
    return ok;
}

bool metricsSnapshotAttempt(std::size_t failIndex, bool expectFailure, std::size_t& allocations)
{
    LocalMetricsService service;
    auto first = std::make_shared<FollowingExporter>();
    auto second = std::make_shared<FollowingExporter>();
    if(!service.addExporter(first) || !service.addExporter(second)) {
        throw std::logic_error("Metrics snapshot exporter setup failed");
    }
    auto name = id<MetricName>("metric.snapshot-allocation-long-identity-outside-small-string-storage");
    std::optional<Result<void>> result;
    bool escaped = false;
    {
        allocation_probe::Scope scope{failIndex};
        try { result.emplace(service.addCounter(std::move(name), 1.0)); }
        catch(const std::bad_alloc&) { escaped = true; }
    }
    allocations = allocation_probe::calls;
    const auto snapshot = service.snapshot();
    bool ok = require(allocation_probe::injected == expectFailure, "metrics snapshot injection", failIndex);
    if(snapshot.empty()) {
        ok &= require(escaped && !result.has_value(), "metrics pre-publication failure is atomic", failIndex);
        ok &= require(first->metricCalls == 0U && second->metricCalls == 0U,
            "metrics unpublished fact is not exported", failIndex);
    } else {
        ok &= require(!escaped && result && result->hasValue(), "metrics published fact returns success", failIndex);
        ok &= require(first->metricCalls == 1U && second->metricCalls == 1U,
            "metrics published fact reaches exporter snapshot", failIndex);
    }
    return ok;
}

bool diagnosticsSnapshotAttempt(std::size_t failIndex, bool expectFailure, std::size_t& allocations)
{
    DiagnosticsService service;
    const auto diagnosticId = id<DiagnosticId>("diagnostic.snapshot-allocation-long-identity-outside-small-string-storage");
    auto first = std::make_shared<FollowingExporter>();
    auto second = std::make_shared<FollowingExporter>();
    if(!service.registerCheck(diagnosticId, std::make_shared<HealthyCheck>(diagnosticId))
       || !service.addExporter(first) || !service.addExporter(second)) {
        throw std::logic_error("Diagnostics snapshot exporter setup failed");
    }
    std::optional<Result<DiagnosticReport>> result;
    bool escaped = false;
    {
        allocation_probe::Scope scope{failIndex};
        try { result.emplace(service.run(diagnosticId)); }
        catch(const std::bad_alloc&) { escaped = true; }
    }
    allocations = allocation_probe::calls;
    const auto latest = service.latest();
    bool ok = require(allocation_probe::injected == expectFailure, "diagnostics snapshot injection", failIndex);
    if(latest.empty()) {
        ok &= require(escaped && !result.has_value(), "diagnostics pre-publication failure is atomic", failIndex);
        ok &= require(first->diagnosticCalls == 0U && second->diagnosticCalls == 0U,
            "diagnostics unpublished report is not exported", failIndex);
    } else {
        ok &= require(!escaped && result && result->hasValue(), "diagnostics published report returns success", failIndex);
        ok &= require(first->diagnosticCalls == 1U && second->diagnosticCalls == 1U,
            "diagnostics published report reaches exporter snapshot", failIndex);
    }
    return ok;
}

template<class Attempt>
bool verifySnapshotPath(Attempt attempt, std::size_t& count)
{
    if(!attempt(std::numeric_limits<std::size_t>::max(), false, count) || count == 0 || count > 1000) {
        return false;
    }
    bool ok = true;
    for(std::size_t index = 0; index < count; ++index) {
        std::size_t observed = 0;
        ok &= attempt(index, true, observed);
        ok &= require(observed == index + 1, "exact snapshot allocation index reached", index);
    }
    std::size_t observed = 0;
    ok &= attempt(count, false, observed);
    ok &= require(observed == count, "stable snapshot allocation path", count);
    return ok;
}

bool verifySnapshotMembership()
{
    bool ok = true;
    {
        LocalTraceService service;
        auto late = std::make_shared<FollowingExporter>();
        auto adding = std::make_shared<AddingTraceExporter>();
        adding->service = &service;
        adding->late = late;
        ok &= require(service.addExporter(adding).hasValue(), "trace adding exporter registered", 0);
        auto first = service.startSpan({id<TraceId>("trace.snapshot-first"),
            id<SpanId>("span.snapshot-first"), {}, "first", {}});
        ok &= require(first.hasValue(), "trace first snapshot span starts", 0);
        if(first) { first.value()->end(TraceStatus::Succeeded); }
        ok &= require(adding->added && late->traceCalls == 0U,
            "trace late exporter excluded from current snapshot", 0);
        auto second = service.startSpan({id<TraceId>("trace.snapshot-second"),
            id<SpanId>("span.snapshot-second"), {}, "second", {}});
        ok &= require(second.hasValue(), "trace second snapshot span starts", 0);
        if(second) { second.value()->end(TraceStatus::Succeeded); }
        ok &= require(late->traceCalls == 1U, "trace late exporter enters next snapshot", 0);
    }
    {
        LocalMetricsService service;
        auto late = std::make_shared<FollowingExporter>();
        auto adding = std::make_shared<AddingMetricsExporter>();
        adding->service = &service;
        adding->late = late;
        ok &= require(service.addExporter(adding).hasValue(), "metrics adding exporter registered", 1);
        ok &= require(service.addCounter(id<MetricName>("metric.snapshot-membership"), 1.0).hasValue(),
            "metrics first snapshot record succeeds", 1);
        ok &= require(adding->added && late->metricCalls == 0U,
            "metrics late exporter excluded from current snapshot", 1);
        ok &= require(service.addCounter(id<MetricName>("metric.snapshot-membership"), 1.0).hasValue(),
            "metrics second snapshot record succeeds", 1);
        ok &= require(late->metricCalls == 1U, "metrics late exporter enters next snapshot", 1);
    }
    {
        DiagnosticsService service;
        const auto diagnosticId = id<DiagnosticId>("diagnostic.snapshot-membership");
        auto late = std::make_shared<FollowingExporter>();
        auto adding = std::make_shared<AddingDiagnosticExporter>();
        adding->service = &service;
        adding->late = late;
        ok &= require(service.registerCheck(
            diagnosticId, std::make_shared<HealthyCheck>(diagnosticId)).hasValue(),
            "diagnostics snapshot check registered", 2);
        ok &= require(service.addExporter(adding).hasValue(), "diagnostics adding exporter registered", 2);
        ok &= require(service.run(diagnosticId).hasValue(), "diagnostics first snapshot run succeeds", 2);
        ok &= require(adding->added && late->diagnosticCalls == 0U,
            "diagnostics late exporter excluded from current snapshot", 2);
        ok &= require(service.run(diagnosticId).hasValue(), "diagnostics second snapshot run succeeds", 2);
        ok &= require(late->diagnosticCalls == 1U,
            "diagnostics late exporter enters next snapshot", 2);
    }
    return ok;
}
}

int main()
{
#ifdef _DEBUG
    // Throwing directly from a replacement allocator can re-enter the MSVC debug-heap lock.
    // Exhaustive injection is therefore certified by Release and ASan processes; Debug still
    // proves that the isolated probe links and starts without changing the Kernel allocator.
    // 中文翻译：MSVC Debug 堆不适合在替换分配器中直接抛出；穷举注入由 Release 与 ASan 进程签核。
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    std::printf("trace-start-allocation-verified debug-link-smoke-only\n");
    return 0;
#else
    try {
        std::size_t count = 0;
        if(!attempt(std::numeric_limits<std::size_t>::max(), count, false) || count == 0 || count > 1000) {
            return 2;
        }
        bool ok = true;
        for(std::size_t index = 0; index < count; ++index) {
            std::size_t observed = 0;
            ok &= attempt(index, observed, true);
            ok &= require(observed == index + 1, "exact allocation index reached", index);
        }
        std::size_t observed = 0;
        ok &= attempt(count, observed, false);
        ok &= require(observed == count, "stable allocation path", count);
        std::size_t validCompletionCount = 0;
        std::size_t invalidCompletionCount = 0;
        std::size_t abandonedCompletionCount = 0;
        ok &= verifyCompletionPath(TraceStatus::Succeeded, validCompletionCount);
        ok &= verifyCompletionPath(static_cast<TraceStatus>(255U), invalidCompletionCount);
        ok &= verifyCompletionPath(TraceStatus::Failed, abandonedCompletionCount, true);
        for(std::size_t scenario = 0; scenario < 4U; ++scenario) {
            ok &= verifyExporterBookkeeping((scenario & 1U) != 0U, (scenario & 2U) != 0U, scenario);
        }
        std::size_t metricsSnapshotCount = 0;
        std::size_t diagnosticsSnapshotCount = 0;
        ok &= verifySnapshotPath(metricsSnapshotAttempt, metricsSnapshotCount);
        ok &= verifySnapshotPath(diagnosticsSnapshotAttempt, diagnosticsSnapshotCount);
        ok &= verifySnapshotMembership();
        if(!ok) { return 3; }
        std::printf("trace-start-allocation-verified start=%zu completion-valid=%zu completion-invalid=%zu completion-abandoned=%zu exporter-bookkeeping=12/12 snapshot-metrics=%zu snapshot-diagnostics=%zu snapshot-membership=3/3 retry-and-state=passed\n",
            count, validCompletionCount, invalidCompletionCount, abandonedCompletionCount,
            metricsSnapshotCount, diagnosticsSnapshotCount);
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr, "trace-allocation-unexpected: %s\n", error.what());
        return 4;
    } catch(...) { return 5; }
#endif
}
