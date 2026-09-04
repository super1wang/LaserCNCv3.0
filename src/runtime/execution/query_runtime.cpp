#include <lasercnc/runtime/query_runtime.hpp>

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/observability/log_service.hpp>
#include <lasercnc/observability/metrics_service.hpp>
#include <lasercnc/observability/trace_service.hpp>
#include <lasercnc/runtime/capability_service.hpp>
#include <lasercnc/runtime/execution_services.hpp>
#include <lasercnc/runtime/document_runtime.hpp>
#include <lasercnc/runtime/query_registry.hpp>
#include <lasercnc/state/document_store.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace lasercnc::runtime {
namespace {

const char* versionResolutionName(VersionResolution resolution) noexcept
{
    switch(resolution) {
    case VersionResolution::Exact: return "exact";
    case VersionResolution::Compatible: return "compatible";
    }
    return "unknown";
}

foundation::Error queryError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const QueryRequest& request,
    std::shared_ptr<const foundation::Error> cause = nullptr)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {foundation::Value::Object {
            {"requestId", foundation::Value {std::string(request.requestId.value())}},
            {"query", foundation::Value {std::string(request.query.value())}},
            {"requestedVersion", foundation::Value {request.version.toString()}},
            {"versionResolution", foundation::Value {
                versionResolutionName(request.versionResolution)}},
        }},
        foundation::Severity::Error,
        std::move(cause));
}

foundation::Result<kernel::SpanId> querySpanId(const kernel::RequestId& requestId)
{
    static std::atomic_ullong sequence {0U};
    return kernel::SpanId::create(
        "span.query." + std::string(requestId.value()) + "."
        + std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed)));
}

void startQuerySpan(
    observability::ITraceService& traces,
    const QueryRequest& request,
    std::unique_ptr<observability::ITraceSpan>& span) noexcept
{
    try {
        auto createdSpanId = querySpanId(request.requestId);
        if(!createdSpanId) {
            return;
        }
        auto started = traces.startSpan(observability::TraceSpanStart {
            request.traceId,
            std::move(createdSpanId).value(),
            request.parentSpanId,
            "query.execute",
            foundation::Value::Object {
                {"query", foundation::Value {std::string(request.query.value())}},
                {"requestedVersion", foundation::Value {request.version.toString()}},
                {"versionResolution", foundation::Value {
                    versionResolutionName(request.versionResolution)}},
            }});
        if(started && started.value() != nullptr) {
            span = std::move(started).value();
        }
    } catch(...) {
    }
}

void recordQueryMetrics(
    observability::IMetricsService& metrics,
    bool succeeded,
    std::chrono::steady_clock::duration elapsed) noexcept
{
    try {
        const observability::MetricLabels labels {
            {"outcome", succeeded ? "success" : "failure"}};
        auto completedMetric = kernel::MetricName::create("kernel.query.completed");
        if(completedMetric) {
            static_cast<void>(metrics.addCounter(
                std::move(completedMetric).value(), 1.0, labels));
        }
        auto durationMetric = kernel::MetricName::create("kernel.query.duration_ms");
        if(durationMetric) {
            const auto duration = std::chrono::duration<double, std::milli>(elapsed).count();
            static_cast<void>(metrics.observeHistogram(
                std::move(durationMetric).value(), duration, labels));
        }
    } catch(...) {
    }
}

observability::LogRecord queryLog(
    const QueryRequest& request,
    observability::LogLevel level,
    const char* outcome,
    const foundation::Version* version = nullptr)
{
    foundation::Value::Object data {
        {"query", foundation::Value {std::string(request.query.value())}},
        {"outcome", foundation::Value {outcome}},
        {"requestedVersion", foundation::Value {request.version.toString()}},
        {"versionResolution", foundation::Value {
            versionResolutionName(request.versionResolution)}},
    };
    if(version != nullptr) {
        data.emplace("version", foundation::Value {version->toString()});
    }
    return observability::LogRecord {
        std::chrono::system_clock::now(),
        level,
        "kernel.runtime",
        "query.execute",
        "Query execution completed",
        observability::LogContext {
            std::string(request.context.sessionId.value()),
            request.context.projectId.has_value()
                ? std::optional<std::string> {
                    std::string(request.context.projectId->value())}
                : std::nullopt,
            std::string(request.requestId.value()),
            std::nullopt,
            std::nullopt,
            std::string(request.correlationId.value()),
            std::string(request.traceId.value())},
        std::move(data)};
}

class ActiveExecution final {
public:
    explicit ActiveExecution(std::atomic_size_t& count) noexcept : count_(count)
    {
        count_.fetch_add(1U, std::memory_order_acq_rel);
    }
    ~ActiveExecution() { count_.fetch_sub(1U, std::memory_order_acq_rel); }

private:
    std::atomic_size_t& count_;
};

} // namespace

class QueryRuntime::Impl final {
public:
    Impl(
        QueryRegistry& queryRegistry,
        state::DocumentStore& documentStore,
        CapabilityService& capabilityService,
        ExecutionServices& services,
        observability::ITraceService& traceService,
        observability::IMetricsService& metricsService,
        DocumentRuntime* documentRuntime)
        : registry(queryRegistry),
          documents(documentStore),
          capabilities(capabilityService),
          executionServices(services),
          traces(traceService),
          metrics(metricsService),
          documentRuntime(documentRuntime)
    {
    }

    void logFailure(
        const ExecutionServicesSnapshot& services,
        const QueryRequest& request,
        const foundation::Version* version) const noexcept
    {
        try {
            static_cast<void>(services.logService->write(queryLog(
                request, observability::LogLevel::Error, "failure", version)));
        } catch(...) {
        }
    }

    foundation::Result<QueryResponse> executeOnce(const QueryRequest& request)
    {
        auto entry = registry.resolve(
            QueryKey {request.query, request.version}, request.versionResolution);
        if(!entry.hasValue()) {
            return foundation::Result<QueryResponse>::failure(std::move(entry).error());
        }
        auto services = executionServices.snapshot();
        if(!services.hasValue()) {
            return foundation::Result<QueryResponse>::failure(std::move(services).error());
        }
        const auto& descriptor = entry.value().descriptor;

        if(!contextMatchesScope(request.context, descriptor.scope)) {
            return foundation::Result<QueryResponse>::failure(queryError(
                "Query.ScopeMismatch",
                foundation::ErrorCategory::Validation,
                "The execution context does not match the query scope",
                request));
        }

        auto argumentsValid = services.value().schemaValidator->validate(
            descriptor.arguments, request.arguments);
        if(!argumentsValid.hasValue()) {
            logFailure(services.value(), request, &descriptor.version);
            return foundation::Result<QueryResponse>::failure(std::move(argumentsValid).error());
        }
        auto authorized = capabilities.authorize(
            request.context.sessionId, descriptor.capability);
        if(!authorized.hasValue()) {
            logFailure(services.value(), request, &descriptor.version);
            return foundation::Result<QueryResponse>::failure(std::move(authorized).error());
        }
        QueryContext context;
        if(request.context.documentId.has_value()) {
            auto document = documents.snapshot(*request.context.documentId);
            if(!document.hasValue()) {
                logFailure(services.value(), request, &descriptor.version);
                return foundation::Result<QueryResponse>::failure(std::move(document).error());
            }
            if(document.value().projectId() != *request.context.projectId) {
                auto error = queryError(
                    "Query.ProjectMismatch",
                    foundation::ErrorCategory::Validation,
                    "The query project does not own the requested document",
                    request);
                logFailure(services.value(), request, &descriptor.version);
                return foundation::Result<QueryResponse>::failure(std::move(error));
            }
            context.document = std::move(document).value();
        }

        foundation::Result<foundation::Value> handled = [&]() {
            try {
                return entry.value().handler->execute(request, context);
            } catch(const std::exception& exception) {
                return foundation::Result<foundation::Value>::failure(queryError(
                    "Query.HandlerFailed",
                    foundation::ErrorCategory::Internal,
                    "The query handler raised an exception",
                    request,
                    std::make_shared<const foundation::Error>(foundation::makeError(
                        "Query.HandlerException",
                        foundation::ErrorCategory::Internal,
                        exception.what()))));
            } catch(...) {
                return foundation::Result<foundation::Value>::failure(queryError(
                    "Query.HandlerFailed",
                    foundation::ErrorCategory::Internal,
                    "The query handler raised an exception",
                    request));
            }
        }();
        if(!handled.hasValue()) {
            logFailure(services.value(), request, &descriptor.version);
            return foundation::Result<QueryResponse>::failure(std::move(handled).error());
        }
        auto resultValid = services.value().schemaValidator->validate(
            descriptor.result, handled.value());
        if(!resultValid.hasValue()) {
            logFailure(services.value(), request, &descriptor.version);
            return foundation::Result<QueryResponse>::failure(std::move(resultValid).error());
        }

        QueryResponse response {
            std::move(handled).value(),
            context.document.has_value()
                ? std::optional<state::RevisionSet> {context.document->revisions()}
                : std::nullopt,
            {},
            descriptor.version,
            descriptor.status};
        try {
            auto logged = services.value().logService->write(queryLog(
                request, observability::LogLevel::Info, "success", &descriptor.version));
            if(!logged.hasValue()) {
                response.postExecutionErrors.push_back(std::move(logged).error());
            }
        } catch(const std::exception& exception) {
            try {
                response.postExecutionErrors.push_back(queryError(
                    "Query.LoggingFailed",
                    foundation::ErrorCategory::Internal,
                    exception.what(),
                    request));
            } catch(...) {
            }
        } catch(...) {
        }
        return foundation::Result<QueryResponse>::success(std::move(response));
    }

    QueryRegistry& registry;
    state::DocumentStore& documents;
    CapabilityService& capabilities;
    ExecutionServices& executionServices;
    observability::ITraceService& traces;
    observability::IMetricsService& metrics;
    DocumentRuntime* documentRuntime;
    std::atomic_bool accepting{false};
    std::atomic_size_t activeExecutions{0U};
};

QueryRuntime::QueryRuntime(
    QueryRegistry& registry,
    state::DocumentStore& documents,
    CapabilityService& capabilities,
    ExecutionServices& executionServices,
    observability::ITraceService& traces,
    observability::IMetricsService& metrics,
    DocumentRuntime* documentRuntime)
    : impl_(std::make_unique<Impl>(
          registry,
          documents,
          capabilities,
          executionServices,
          traces,
          metrics,
          documentRuntime))
{
}

QueryRuntime::~QueryRuntime() = default;

foundation::Result<QueryResponse> QueryRuntime::execute(const QueryRequest& request)
{
    return executeObserved(request, false);
}

foundation::Result<QueryResponse> QueryRuntime::executeObserved(
    const QueryRequest& request, bool kernelRejected)
{
    const auto startedAt = std::chrono::steady_clock::now();
    std::unique_ptr<observability::ITraceSpan> span;
    startQuerySpan(impl_->traces, request, span);
    std::optional<DocumentActivityLease> documentActivity;
    std::optional<ProjectActivityLease> projectActivity;

    auto observedResult = [&]() -> foundation::Result<QueryResponse> {
        if(kernelRejected || !impl_->accepting.load(std::memory_order_acquire)) {
            return foundation::Result<QueryResponse>::failure(queryError(
                "Query.RuntimeNotReady",
                foundation::ErrorCategory::Conflict,
                "The query runtime is not accepting requests",
                request));
        }
        if(impl_->documentRuntime != nullptr
           && request.context.documentId.has_value()) {
            auto admitted = impl_->documentRuntime->acquireActivity(
                *request.context.documentId, DocumentActivityKind::Query);
            if(!admitted) {
                return foundation::Result<QueryResponse>::failure(
                    std::move(admitted).error());
            }
            documentActivity.emplace(std::move(admitted).value());
        } else if(impl_->documentRuntime != nullptr && request.context.projectId.has_value()) {
            auto admitted = impl_->documentRuntime->acquireProjectActivity(*request.context.projectId);
            if(!admitted) { return foundation::Result<QueryResponse>::failure(std::move(admitted).error()); }
            projectActivity.emplace(std::move(admitted).value());
        }
        ActiveExecution active(impl_->activeExecutions);
        try {
            return impl_->executeOnce(request);
        } catch(const std::exception& exception) {
            return foundation::Result<QueryResponse>::failure(queryError(
                "Query.ExecutionFailed",
                foundation::ErrorCategory::Internal,
                exception.what(),
                request));
        } catch(...) {
            return foundation::Result<QueryResponse>::failure(queryError(
                "Query.ExecutionFailed",
                foundation::ErrorCategory::Internal,
                "Query execution failed unexpectedly",
                request));
        }
    }();

    const auto succeeded = observedResult.hasValue();
    if(span != nullptr) {
        span->end(
            succeeded ? observability::TraceStatus::Succeeded
                      : observability::TraceStatus::Failed,
            succeeded ? std::nullopt
                      : std::optional<foundation::Error> {observedResult.error()});
    }
    recordQueryMetrics(
        impl_->metrics, succeeded, std::chrono::steady_clock::now() - startedAt);
    return observedResult;
}

std::size_t QueryRuntime::activeExecutionCount() const noexcept
{
    return impl_->activeExecutions.load(std::memory_order_acquire);
}

bool QueryRuntime::accepting() const noexcept
{
    return impl_->accepting.load(std::memory_order_acquire);
}

void QueryRuntime::start() noexcept
{
    impl_->accepting.store(true, std::memory_order_release);
}

void QueryRuntime::stop() noexcept
{
    impl_->accepting.store(false, std::memory_order_release);
}

} // namespace lasercnc::runtime
