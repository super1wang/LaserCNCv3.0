#pragma once

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/value.hpp>
#include <lasercnc/kernel/identifiers.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lasercnc::observability {

enum class TraceStatus : std::uint8_t {
    Running,
    Succeeded,
    Failed,
    Cancelled,
    Stale
};

struct TraceSpanStart final {
    kernel::TraceId traceId;
    kernel::SpanId spanId;
    std::optional<kernel::SpanId> parentSpanId;
    std::string name;
    foundation::Value::Object attributes;
};

struct TraceSpanRecord final {
    kernel::TraceId traceId;
    kernel::SpanId spanId;
    std::optional<kernel::SpanId> parentSpanId;
    std::string name;
    std::chrono::system_clock::time_point startedAt;
    std::chrono::system_clock::time_point finishedAt;
    TraceStatus status{TraceStatus::Running};
    foundation::Value::Object attributes;
    std::optional<foundation::Error> error;
};

class ITraceExporter {
public:
    virtual ~ITraceExporter() = default;
    [[nodiscard]] virtual foundation::Result<void> exportSpan(
        const TraceSpanRecord& span) = 0;
};

class ITraceSpan {
public:
    virtual ~ITraceSpan() = default;
    [[nodiscard]] virtual const kernel::TraceId& traceId() const noexcept = 0;
    [[nodiscard]] virtual const kernel::SpanId& spanId() const noexcept = 0;
    virtual void end(
        TraceStatus status,
        std::optional<foundation::Error> error = std::nullopt) noexcept = 0;
};

class ITraceService {
public:
    virtual ~ITraceService() = default;
    [[nodiscard]] virtual foundation::Result<std::unique_ptr<ITraceSpan>> startSpan(
        TraceSpanStart start) = 0;
};

class LocalTraceService final : public ITraceService {
public:
    explicit LocalTraceService(
        std::size_t recordCapacity = 4096U,
        std::size_t failureCapacity = 256U);
    ~LocalTraceService() override;

    LocalTraceService(const LocalTraceService&) = delete;
    LocalTraceService& operator=(const LocalTraceService&) = delete;

    [[nodiscard]] foundation::Result<void> addExporter(
        std::shared_ptr<ITraceExporter> exporter);
    void freeze();
    [[nodiscard]] bool frozen() const;
    [[nodiscard]] foundation::Result<std::unique_ptr<ITraceSpan>> startSpan(
        TraceSpanStart start) override;
    [[nodiscard]] std::vector<TraceSpanRecord> records() const;
    [[nodiscard]] std::vector<foundation::Error> exporterFailures() const;
    [[nodiscard]] std::size_t activeSpanCount() const;

private:
    struct Core;
    std::shared_ptr<Core> core_;
};

} // namespace lasercnc::observability
