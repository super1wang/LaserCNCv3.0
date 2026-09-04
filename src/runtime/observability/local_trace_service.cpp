#include <lasercnc/observability/trace_service.hpp>

#include <lasercnc/foundation/error.hpp>

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <utility>

namespace lasercnc::observability {
namespace {

foundation::Error traceError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const kernel::SpanId& spanId)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {foundation::Value::Object {
            {"spanId", foundation::Value {std::string(spanId.value())}},
        }});
}

} // namespace

struct LocalTraceService::Core final {
    Core(std::size_t records, std::size_t failures)
        : recordCapacity(std::max<std::size_t>(records, 1U)),
          failureCapacity(std::max<std::size_t>(failures, 1U))
    {
    }

    std::mutex mutex;
    std::map<kernel::SpanId, TraceSpanRecord> active;
    std::vector<TraceSpanRecord> completed;
    std::vector<std::shared_ptr<ITraceExporter>> exporters;
    std::vector<foundation::Error> failures;
    std::size_t recordCapacity;
    std::size_t failureCapacity;
    bool frozen{false};
};

LocalTraceService::LocalTraceService(
    std::size_t recordCapacity,
    std::size_t failureCapacity)
    : core_(std::make_shared<Core>(recordCapacity, failureCapacity))
{
}
LocalTraceService::~LocalTraceService() = default;

foundation::Result<void> LocalTraceService::addExporter(
    std::shared_ptr<ITraceExporter> exporter)
{
    if(exporter == nullptr) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Trace.InvalidExporter",
            foundation::ErrorCategory::Validation,
            "A trace exporter is required"));
    }
    std::lock_guard lock(core_->mutex);
    if(core_->frozen) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Trace.ExportersFrozen",
            foundation::ErrorCategory::Conflict,
            "Trace exporters are frozen"));
    }
    core_->exporters.push_back(std::move(exporter));
    return foundation::Result<void>::success();
}

void LocalTraceService::freeze()
{
    std::lock_guard lock(core_->mutex);
    core_->frozen = true;
}

bool LocalTraceService::frozen() const
{
    std::lock_guard lock(core_->mutex);
    return core_->frozen;
}

foundation::Result<std::unique_ptr<ITraceSpan>> LocalTraceService::startSpan(
    TraceSpanStart start)
{
    if(start.name.empty()) {
        return foundation::Result<std::unique_ptr<ITraceSpan>>::failure(traceError(
            "Trace.InvalidSpanName",
            foundation::ErrorCategory::Validation,
            "A trace span name is required",
            start.spanId));
    }
    const auto spanId = start.spanId;
    const auto traceId = start.traceId;
    using Completion = std::function<void(TraceStatus, std::optional<foundation::Error>)>;
    Completion completion = [core = core_, spanId](
                                TraceStatus status,
                                std::optional<foundation::Error> error) {
        std::optional<TraceSpanRecord> completed;
        std::vector<std::shared_ptr<ITraceExporter>> exporters;
        {
            std::lock_guard lock(core->mutex);
            const auto found = core->active.find(spanId);
            if(found == core->active.end()) {
                return;
            }
            found->second.finishedAt = std::chrono::system_clock::now();
            found->second.status = status;
            found->second.error = std::move(error);
            completed = std::move(found->second);
            core->active.erase(found);
            if(core->completed.size() >= core->recordCapacity) {
                core->completed.erase(core->completed.begin());
            }
            core->completed.push_back(*completed);
            exporters = core->exporters;
        }

        const auto retainFailure = [&core](foundation::Error failure) noexcept {
            try {
                std::lock_guard lock(core->mutex);
                if(core->failures.size() >= core->failureCapacity) {
                    core->failures.erase(core->failures.begin());
                }
                core->failures.push_back(std::move(failure));
            } catch(...) {
            }
        };
        for(const auto& exporter : exporters) {
            try {
                auto exported = exporter->exportSpan(*completed);
                if(!exported) {
                    retainFailure(std::move(exported).error());
                }
            } catch(const std::exception& exception) {
                try {
                    retainFailure(traceError(
                        "Trace.ExporterThrew",
                        foundation::ErrorCategory::Internal,
                        exception.what(),
                        spanId));
                } catch(...) {
                }
            } catch(...) {
                try {
                    retainFailure(traceError(
                        "Trace.ExporterThrew",
                        foundation::ErrorCategory::Internal,
                        "A trace exporter raised an unknown exception",
                        spanId));
                } catch(...) {
                }
            }
        }
    };

    class Span final : public ITraceSpan {
    public:
        Span(
            kernel::TraceId traceId,
            kernel::SpanId spanId,
            std::shared_ptr<Core> core,
            Completion completion)
            : traceId_(std::move(traceId)),
              spanId_(std::move(spanId)),
              core_(std::move(core)),
              completion_(std::move(completion))
        {
        }

        ~Span() override
        {
            if(armed_ && !ended_.exchange(true, std::memory_order_acq_rel)) {
                try {
                    completion_(TraceStatus::Failed, traceError(
                        "Trace.SpanAbandoned",
                        foundation::ErrorCategory::Internal,
                        "A trace span was destroyed before explicit completion",
                        spanId_));
                } catch(...) {
                    discardActive();
                }
            }
        }

        const kernel::TraceId& traceId() const noexcept override { return traceId_; }
        const kernel::SpanId& spanId() const noexcept override { return spanId_; }
        void arm() noexcept { armed_ = true; }

        void end(TraceStatus status, std::optional<foundation::Error> error) noexcept override
        {
            if(ended_.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            try {
                switch(status) {
                case TraceStatus::Succeeded:
                case TraceStatus::Failed:
                case TraceStatus::Cancelled:
                case TraceStatus::Stale:
                    break;
                default:
                    error = foundation::makeError(
                        "Trace.InvalidTerminalStatus", foundation::ErrorCategory::Validation,
                        "Trace completion requires a declared terminal status",
                        foundation::Value{foundation::Value::Object{
                            {"spanId", foundation::Value{std::string(spanId_.value())}},
                            {"requestedStatus", foundation::Value{static_cast<std::int64_t>(status)}}}},
                        foundation::Severity::Error,
                        error ? std::make_shared<const foundation::Error>(std::move(*error)) : nullptr);
                    status = TraceStatus::Failed;
                    break;
                }
                completion_(status, std::move(error));
            } catch(...) {
                // A noexcept handle must still surrender its active identity if diagnostics,
                // record retention, or exporter snapshot allocation fails.
                // 中文翻译：即使诊断、记录或出口快照分配失败，noexcept 句柄也必须释放活动身份。
                discardActive();
            }
        }

    private:
        void discardActive() noexcept
        {
            try {
                std::lock_guard lock(core_->mutex);
                core_->active.erase(spanId_);
            } catch(...) {
                // Lock failures are not recoverable inside a noexcept completion handle.
                // 中文翻译：锁获取失败无法在 noexcept 完成句柄内部恢复。
            }
        }

        kernel::TraceId traceId_;
        kernel::SpanId spanId_;
        std::shared_ptr<Core> core_;
        Completion completion_;
        std::atomic_bool ended_{false};
        bool armed_{false};
    };

    // Allocate every fallible handle resource before publishing the active identity. A disarmed
    // handle is inert if allocation or duplicate admission fails; after insertion no allocation
    // is needed to transfer its ownership into Result.
    // 中文翻译：先完成句柄全部可失败分配，再发布活动身份；未准入的句柄析构不会生成虚假完成。
    auto span = std::make_unique<Span>(traceId, spanId, core_, std::move(completion));
    {
        std::lock_guard lock(core_->mutex);
        const auto completed = std::find_if(
            core_->completed.begin(),
            core_->completed.end(),
            [&spanId](const TraceSpanRecord& record) { return record.spanId == spanId; });
        if(core_->active.contains(spanId) || completed != core_->completed.end()) {
            return foundation::Result<std::unique_ptr<ITraceSpan>>::failure(traceError(
                "Trace.SpanIdAlreadyExists",
                foundation::ErrorCategory::Conflict,
                "A trace span id is reserved while active or retained",
                spanId));
        }
        const auto now = std::chrono::system_clock::now();
        core_->active.emplace(
            spanId,
            TraceSpanRecord {
                std::move(start.traceId),
                start.spanId,
                std::move(start.parentSpanId),
                std::move(start.name),
                now,
                now,
                TraceStatus::Running,
                std::move(start.attributes),
                std::nullopt});
        span->arm();
    }
    return foundation::Result<std::unique_ptr<ITraceSpan>>::success(
        std::move(span));
}

std::vector<TraceSpanRecord> LocalTraceService::records() const
{
    std::lock_guard lock(core_->mutex);
    return core_->completed;
}

std::vector<foundation::Error> LocalTraceService::exporterFailures() const
{
    std::lock_guard lock(core_->mutex);
    return core_->failures;
}

std::size_t LocalTraceService::activeSpanCount() const
{
    std::lock_guard lock(core_->mutex);
    return core_->active.size();
}

} // namespace lasercnc::observability
