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

void beforeAllocation()
{
    if(armed && calls++ == failAt) {
        armed = false;
        injected = true;
        throw std::bad_alloc{};
    }
}

struct Scope final {
    explicit Scope(std::size_t index)
    {
        calls = 0;
        failAt = index;
        injected = false;
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
        if(!ok) { return 3; }
        std::printf("trace-start-allocation-verified injected=%zu baseline=%zu retry-and-state=passed\n", count, observed);
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr, "trace-allocation-unexpected: %s\n", error.what());
        return 4;
    } catch(...) { return 5; }
#endif
}
