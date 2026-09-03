#include <lasercnc/runtime/command_runtime.hpp>

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/messaging/event_bus.hpp>
#include <lasercnc/observability/log_service.hpp>
#include <lasercnc/observability/metrics_service.hpp>
#include <lasercnc/observability/trace_service.hpp>
#include <lasercnc/persistence/persistence_service.hpp>
#include <lasercnc/runtime/capability_service.hpp>
#include <lasercnc/runtime/command_registry.hpp>
#include <lasercnc/runtime/document_runtime.hpp>
#include <lasercnc/runtime/execution_services.hpp>
#include <lasercnc/runtime/effect_executor.hpp>
#include <lasercnc/runtime/transaction_manager.hpp>
#include <lasercnc/runtime/task_runtime.hpp>
#include <lasercnc/state/document_store.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>

namespace lasercnc::runtime {
namespace {

bool isExternalSideEffect(SideEffectLevel sideEffect) noexcept
{
    return sideEffect != SideEffectLevel::ReadOnly
        && sideEffect != SideEffectLevel::DocumentWrite;
}

foundation::Error commandError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const CommandRequest& request,
    std::shared_ptr<const foundation::Error> cause = nullptr)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {foundation::Value::Object {
            {"requestId", foundation::Value {std::string(request.requestId.value())}},
            {"command", foundation::Value {std::string(request.command.value())}},
            {"requestedVersion", foundation::Value {request.version.toString()}},
            {"versionResolution", foundation::Value {
                request.versionResolution == VersionResolution::Exact ? "exact" : "compatible"}},
        }},
        foundation::Severity::Error,
        std::move(cause));
}

foundation::Result<kernel::SpanId> commandSpanId(const kernel::RequestId& requestId)
{
    static std::atomic_ullong sequence {0U};
    return kernel::SpanId::create(
        "span.command." + std::string(requestId.value()) + "."
        + std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed)));
}

void startCommandSpan(
    observability::ITraceService& traces,
    const CommandRequest& request,
    std::optional<kernel::SpanId>& activeSpanId,
    std::unique_ptr<observability::ITraceSpan>& span) noexcept
{
    try {
        auto createdSpanId = commandSpanId(request.requestId);
        if(!createdSpanId) {
            return;
        }
        const auto spanId = createdSpanId.value();
        auto started = traces.startSpan(observability::TraceSpanStart {
            request.traceId,
            spanId,
            request.parentSpanId,
            "command.execute",
            foundation::Value::Object {
                {"command", foundation::Value {std::string(request.command.value())}},
                {"requestedVersion", foundation::Value {request.version.toString()}},
                {"versionResolution", foundation::Value {
                    request.versionResolution == VersionResolution::Exact
                        ? "exact" : "compatible"}},
            }});
        if(started && started.value() != nullptr) {
            activeSpanId = spanId;
            span = std::move(started).value();
        }
    } catch(...) {
    }
}

void recordCommandMetrics(
    observability::IMetricsService& metrics,
    bool succeeded,
    std::chrono::steady_clock::duration elapsed) noexcept
{
    try {
        const observability::MetricLabels labels {
            {"outcome", succeeded ? "success" : "failure"}};
        auto completedMetric = kernel::MetricName::create("kernel.command.completed");
        if(completedMetric) {
            static_cast<void>(metrics.addCounter(
                std::move(completedMetric).value(), 1.0, labels));
        }
        auto durationMetric = kernel::MetricName::create("kernel.command.duration_ms");
        if(durationMetric) {
            const auto duration = std::chrono::duration<double, std::milli>(elapsed).count();
            static_cast<void>(metrics.observeHistogram(
                std::move(durationMetric).value(), duration, labels));
        }
    } catch(...) {
    }
}

struct RequestSignature final {
    ExecutionContext context;
    kernel::CommandName command;
    foundation::Version version;
    VersionResolution versionResolution{VersionResolution::Exact};
    foundation::Value arguments;
    std::optional<state::Revision> expectedRevision;

    friend bool operator==(const RequestSignature&, const RequestSignature&) = default;
};

RequestSignature signatureOf(const CommandRequest& request)
{
    return RequestSignature {
        request.context,
        request.command,
        request.version,
        request.versionResolution,
        request.arguments,
        request.expectedRevision};
}

foundation::Value persistentSignature(
    const CommandRequest& request,
    const CommandDescriptor& descriptor)
{
    return foundation::Value {foundation::Value::Object {
        {"arguments", request.arguments},
        {"command", foundation::Value {std::string(request.command.value())}},
        {"documentId", request.context.documentId.has_value()
            ? foundation::Value {std::string(request.context.documentId->value())}
            : foundation::Value {}},
        {"expectedProjectRevision", request.expectedRevision.has_value()
            ? foundation::Value {std::to_string(request.expectedRevision->value())}
            : foundation::Value {}},
        {"format", foundation::Value {"lasercnc.command-signature.v2"}},
        {"projectId", request.context.projectId.has_value()
            ? foundation::Value {std::string(request.context.projectId->value())}
            : foundation::Value {}},
        {"sessionId", foundation::Value {std::string(request.context.sessionId.value())}},
        {"requestedVersion", foundation::Value {request.version.toString()}},
        {"resolvedVersion", foundation::Value {descriptor.version.toString()}},
        {"versionResolution", foundation::Value {
            request.versionResolution == VersionResolution::Exact ? "exact" : "compatible"}},
    }};
}

class PersistentIdempotencyLease final {
public:
    PersistentIdempotencyLease(
        persistence::PersistenceService& persistence,
        kernel::IdempotencyKey key,
        foundation::Value signature)
        : persistence_(&persistence), key_(std::move(key)), signature_(std::move(signature))
    {
    }

    ~PersistentIdempotencyLease()
    {
        if(active_) {
            try {
                static_cast<void>(persistence_->releaseCommandClaim(key_, signature_));
            } catch(...) {
            }
        }
    }

    PersistentIdempotencyLease(const PersistentIdempotencyLease&) = delete;
    PersistentIdempotencyLease& operator=(const PersistentIdempotencyLease&) = delete;

    void complete() noexcept { active_ = false; }
    [[nodiscard]] const foundation::Value& signature() const noexcept { return signature_; }

private:
    persistence::PersistenceService* persistence_;
    kernel::IdempotencyKey key_;
    foundation::Value signature_;
    bool active_{true};
};

observability::LogRecord commandLog(
    const CommandRequest& request,
    observability::LogLevel level,
    const char* outcome,
    const foundation::Version* version = nullptr)
{
    foundation::Value::Object data {
        {"command", foundation::Value {std::string(request.command.value())}},
        {"outcome", foundation::Value {outcome}},
        {"requestedVersion", foundation::Value {request.version.toString()}},
        {"versionResolution", foundation::Value {
            request.versionResolution == VersionResolution::Exact ? "exact" : "compatible"}},
    };
    if(version != nullptr) {
        data.emplace("version", foundation::Value {version->toString()});
    }
    return observability::LogRecord {
        std::chrono::system_clock::now(),
        level,
        "kernel.runtime",
        "command.execute",
        "Command execution completed",
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

class CommandRuntime::Impl final {
public:
    Impl(
        CommandRegistry& commandRegistry,
        const state::DocumentStore& documentStore,
        EffectExecutor& effectExecutor,
        TransactionManager& transactionManager,
        CapabilityService& capabilityService,
        messaging::EventBus& eventBus,
        ExecutionServices& services,
        TaskRuntime& taskRuntime,
        persistence::PersistenceService& persistenceService,
        observability::ITraceService& traceService,
        observability::IMetricsService& metricsService,
        std::size_t capacity,
        DocumentRuntime* documentRuntime)
        : registry(commandRegistry),
          documents(documentStore),
          effects(effectExecutor),
          transactions(transactionManager),
          capabilities(capabilityService),
          events(eventBus),
          executionServices(services),
          tasks(taskRuntime),
          persistence(persistenceService),
          traces(traceService),
          metrics(metricsService),
          documentRuntime(documentRuntime),
          idempotencyCapacity(capacity)
    {
    }

    struct IdempotencyRecord final {
        IdempotencyRecord(RequestSignature requestSignature, std::thread::id ownerThread)
            : signature(std::move(requestSignature)), owner(ownerThread)
        {
        }

        RequestSignature signature;
        std::thread::id owner;
        bool complete{false};
        bool abandoned{false};
        std::optional<foundation::Result<CommandResponse>> result;
        std::condition_variable completed;
    };

    foundation::Result<CommandResponse> executeSafe(
        const CommandRequest& request,
        std::optional<kernel::SpanId> activeSpanId)
    {
        try {
            return executeOnce(request, std::move(activeSpanId));
        } catch(const std::exception& exception) {
            return foundation::Result<CommandResponse>::failure(commandError(
                "Command.ExecutionFailed",
                foundation::ErrorCategory::Internal,
                "Command execution failed unexpectedly",
                request,
                std::make_shared<const foundation::Error>(foundation::makeError(
                    "Command.Exception",
                    foundation::ErrorCategory::Internal,
                    exception.what()))));
        } catch(...) {
            return foundation::Result<CommandResponse>::failure(commandError(
                "Command.ExecutionFailed",
                foundation::ErrorCategory::Internal,
                "Command execution failed unexpectedly",
                request));
        }
    }

    void logFailure(
        const ExecutionServicesSnapshot& services,
        const CommandRequest& request,
        const foundation::Version* version) const noexcept
    {
        try {
            static_cast<void>(services.logService->write(commandLog(
                request, observability::LogLevel::Error, "failure", version)));
        } catch(...) {
        }
    }

    bool isExternalEffectRequest(const CommandRequest& request) const
    {
        auto entry = registry.resolve(
            CommandKey {request.command, request.version}, request.versionResolution);
        return entry && isExternalSideEffect(entry.value().descriptor.sideEffect);
    }

    foundation::Result<CommandResponse> executeOnce(
        const CommandRequest& request,
        std::optional<kernel::SpanId> activeSpanId)
    {
        auto entry = registry.resolve(
            CommandKey {request.command, request.version}, request.versionResolution);
        if(!entry.hasValue()) {
            return foundation::Result<CommandResponse>::failure(std::move(entry).error());
        }

        auto services = executionServices.snapshot();
        if(!services.hasValue()) {
            return foundation::Result<CommandResponse>::failure(std::move(services).error());
        }
        const auto& descriptor = entry.value().descriptor;

        if(!contextMatchesScope(request.context, descriptor.scope)) {
            return foundation::Result<CommandResponse>::failure(commandError(
                "Command.ScopeMismatch",
                foundation::ErrorCategory::Validation,
                "The execution context does not match the command scope",
                request));
        }

        auto argumentsValid = services.value().schemaValidator->validate(
            descriptor.arguments, request.arguments);
        if(!argumentsValid.hasValue()) {
            logFailure(services.value(), request, &descriptor.version);
            return foundation::Result<CommandResponse>::failure(std::move(argumentsValid).error());
        }
        if(request.idempotencyKey.has_value() && !descriptor.idempotent) {
            auto error = commandError(
                "Command.IdempotencyUnsupported",
                foundation::ErrorCategory::Validation,
                "The command does not declare idempotent replay semantics",
                request);
            logFailure(services.value(), request, &descriptor.version);
            return foundation::Result<CommandResponse>::failure(std::move(error));
        }
        auto authorized = capabilities.authorize(
            request.context.sessionId, descriptor.capability);
        if(!authorized.hasValue()) {
            logFailure(services.value(), request, &descriptor.version);
            return foundation::Result<CommandResponse>::failure(std::move(authorized).error());
        }

        if(isExternalSideEffect(descriptor.sideEffect)) {
            auto executed = effects.execute(
                request,
                descriptor,
                entry.value().externalEffectHandler,
                *services.value().schemaValidator);
            if(!executed) {
                logFailure(services.value(), request, &descriptor.version);
                return foundation::Result<CommandResponse>::failure(
                    std::move(executed).error());
            }
            auto effect = std::move(executed).value();
            CommandResponse response {
                std::move(effect.result),
                std::nullopt,
                std::nullopt,
                {},
                effect.replayed,
                descriptor.version,
                descriptor.status,
                effect.disposition};
            try {
                auto logged = services.value().logService->write(commandLog(
                    request,
                    observability::LogLevel::Info,
                    effect.replayed ? "replayed" : "success",
                    &descriptor.version));
                if(!logged) {
                    response.postExecutionErrors.push_back(std::move(logged).error());
                }
            } catch(const std::exception& exception) {
                try {
                    response.postExecutionErrors.push_back(commandError(
                        "Command.PostExecutionIntegrationFailed",
                        foundation::ErrorCategory::Internal,
                        exception.what(),
                        request));
                } catch(...) {
                }
            } catch(...) {
            }
            return foundation::Result<CommandResponse>::success(std::move(response));
        }

        if(descriptor.executionMode == ExecutionMode::Synchronous
           && descriptor.sideEffect == SideEffectLevel::ReadOnly) {
            ReadOnlyCommandContext context;
            if(request.context.documentId.has_value()) {
                auto document = documents.snapshot(*request.context.documentId);
                if(!document) {
                    logFailure(services.value(), request, &descriptor.version);
                    return foundation::Result<CommandResponse>::failure(
                        std::move(document).error());
                }
                if(document.value().projectId() != *request.context.projectId) {
                    auto error = commandError(
                        "Command.ProjectMismatch",
                        foundation::ErrorCategory::Validation,
                        "The command project does not own the target document",
                        request);
                    logFailure(services.value(), request, &descriptor.version);
                    return foundation::Result<CommandResponse>::failure(std::move(error));
                }
                if(request.expectedRevision.has_value()
                   && document.value().revisions().at(state::RevisionScope::Project)
                       != *request.expectedRevision) {
                    auto error = commandError(
                        "Command.RevisionConflict",
                        foundation::ErrorCategory::Conflict,
                        "The command project revision does not match the caller precondition",
                        request);
                    logFailure(services.value(), request, &descriptor.version);
                    return foundation::Result<CommandResponse>::failure(std::move(error));
                }
                context.document = std::move(document).value();
            } else if(request.expectedRevision.has_value()) {
                auto error = commandError(
                    "Command.RevisionScopeMismatch",
                    foundation::ErrorCategory::Validation,
                    "Revision preconditions require document scope",
                    request);
                logFailure(services.value(), request, &descriptor.version);
                return foundation::Result<CommandResponse>::failure(std::move(error));
            }

            foundation::Result<foundation::Value> handled = [&]() {
                try {
                    return entry.value().readOnlyHandler->execute(request, context);
                } catch(const std::exception& exception) {
                    return foundation::Result<foundation::Value>::failure(commandError(
                        "Command.HandlerFailed",
                        foundation::ErrorCategory::Internal,
                        "The read-only command handler raised an exception",
                        request,
                        std::make_shared<const foundation::Error>(foundation::makeError(
                            "Command.HandlerException",
                            foundation::ErrorCategory::Internal,
                            exception.what()))));
                } catch(...) {
                    return foundation::Result<foundation::Value>::failure(commandError(
                        "Command.HandlerFailed",
                        foundation::ErrorCategory::Internal,
                        "The read-only command handler raised an exception",
                        request));
                }
            }();
            if(!handled) {
                logFailure(services.value(), request, &descriptor.version);
                return foundation::Result<CommandResponse>::failure(
                    std::move(handled).error());
            }
            auto resultValid = services.value().schemaValidator->validate(
                descriptor.result, handled.value());
            if(!resultValid) {
                logFailure(services.value(), request, &descriptor.version);
                return foundation::Result<CommandResponse>::failure(
                    std::move(resultValid).error());
            }
            CommandResponse response {
                std::move(handled).value(),
                std::nullopt,
                std::nullopt,
                {},
                false,
                descriptor.version,
                descriptor.status};
            try {
                auto logged = services.value().logService->write(commandLog(
                    request, observability::LogLevel::Info, "success", &descriptor.version));
                if(!logged) {
                    response.postExecutionErrors.push_back(std::move(logged).error());
                }
            } catch(const std::exception& exception) {
                try {
                    response.postExecutionErrors.push_back(commandError(
                        "Command.PostExecutionIntegrationFailed",
                        foundation::ErrorCategory::Internal,
                        exception.what(),
                        request));
                } catch(...) {
                }
            } catch(...) {
            }
            return foundation::Result<CommandResponse>::success(std::move(response));
        }

        std::unique_ptr<PersistentIdempotencyLease> durableLease;
        if(request.idempotencyKey.has_value() && persistence.configured()) {
            auto signature = persistentSignature(request, descriptor);
            auto claim = persistence.claimCommand(*request.idempotencyKey, signature);
            if(!claim) {
                const auto code = std::string(claim.error().code.value());
                if(code == "Persistence.IdempotencyKeyConflict") {
                    return foundation::Result<CommandResponse>::failure(commandError(
                        "Command.IdempotencyKeyConflict",
                        foundation::ErrorCategory::Conflict,
                        "The idempotency key is already bound to a different request",
                        request,
                        std::make_shared<const foundation::Error>(
                            std::move(claim).error())));
                }
                return foundation::Result<CommandResponse>::failure(commandError(
                    "Command.IdempotencyPersistenceFailed",
                    foundation::ErrorCategory::Infrastructure,
                    "The command could not acquire its durable idempotency record",
                    request,
                    std::make_shared<const foundation::Error>(
                        std::move(claim).error())));
            }
            if(claim.value().disposition
               == persistence::IdempotencyClaimDisposition::Replayed) {
                if(!claim.value().replay.has_value()) {
                    return foundation::Result<CommandResponse>::failure(commandError(
                        "Command.IdempotencyRecordLost",
                        foundation::ErrorCategory::Internal,
                        "A durable replay did not contain an outcome",
                        request));
                }
                auto replay = std::move(*claim.value().replay);
                auto replayValid = services.value().schemaValidator->validate(
                    descriptor.result, replay.result);
                if(!replayValid) {
                    return foundation::Result<CommandResponse>::failure(commandError(
                        "Command.IdempotencyReplayInvalid",
                        foundation::ErrorCategory::Infrastructure,
                        "A durable replay no longer satisfies the command result schema",
                        request,
                        std::make_shared<const foundation::Error>(
                            std::move(replayValid).error())));
                }
                return foundation::Result<CommandResponse>::success(CommandResponse {
                    std::move(replay.result),
                    std::move(replay.commit),
                    std::move(replay.taskId),
                    {},
                    true,
                    descriptor.version,
                    descriptor.status});
            }
            durableLease = std::make_unique<PersistentIdempotencyLease>(
                persistence, *request.idempotencyKey, std::move(signature));
        }

        if(descriptor.executionMode == ExecutionMode::Asynchronous) {
            foundation::Result<AsyncCommandPlan> prepared = [&]() {
                try {
                    return entry.value().asyncHandler->prepare(request);
                } catch(const std::exception& exception) {
                    return foundation::Result<AsyncCommandPlan>::failure(commandError(
                        "Command.HandlerFailed",
                        foundation::ErrorCategory::Internal,
                        "The asynchronous command handler raised an exception",
                        request,
                        std::make_shared<const foundation::Error>(foundation::makeError(
                            "Command.HandlerException",
                            foundation::ErrorCategory::Internal,
                            exception.what()))));
                } catch(...) {
                    return foundation::Result<AsyncCommandPlan>::failure(commandError(
                        "Command.HandlerFailed",
                        foundation::ErrorCategory::Internal,
                        "The asynchronous command handler raised an exception",
                        request));
                }
            }();
            if(!prepared) {
                logFailure(services.value(), request, &descriptor.version);
                return foundation::Result<CommandResponse>::failure(std::move(prepared).error());
            }

            auto plan = std::move(prepared).value();
            auto resultValid = services.value().schemaValidator->validate(
                descriptor.result, plan.acceptance);
            if(!resultValid) {
                logFailure(services.value(), request, &descriptor.version);
                return foundation::Result<CommandResponse>::failure(std::move(resultValid).error());
            }
            plan.task.traceId = request.traceId;
            plan.task.correlationId = request.correlationId;
            plan.task.projectId = request.context.projectId;
            plan.task.documentId = request.context.documentId;
            plan.task.expectedProjectRevision = request.expectedRevision;
            plan.task.parentSpanId = activeSpanId.has_value()
                ? activeSpanId
                : request.parentSpanId;
            const auto taskId = plan.task.taskId;
            std::optional<TransactionIdempotency> durableIdempotency;
            if(durableLease != nullptr) {
                durableIdempotency = TransactionIdempotency {
                    *request.idempotencyKey,
                    durableLease->signature(),
                    plan.acceptance};
            }
            CommandResponse response {
                std::move(plan.acceptance),
                std::nullopt,
                taskId,
                {},
                false,
                descriptor.version,
                descriptor.status};
            auto submitted = tasks.submit(
                std::move(plan.task), std::move(durableIdempotency));
            if(!submitted) {
                logFailure(services.value(), request, &descriptor.version);
                return foundation::Result<CommandResponse>::failure(std::move(submitted).error());
            }
            if(durableLease != nullptr) {
                durableLease->complete();
            }
            try {
                auto logged = services.value().logService->write(commandLog(
                    request, observability::LogLevel::Info, "accepted", &descriptor.version));
                if(!logged) {
                    response.postExecutionErrors.push_back(std::move(logged).error());
                }
            } catch(const std::exception& exception) {
                try {
                    response.postExecutionErrors.push_back(commandError(
                        "Command.PostAcceptanceIntegrationFailed",
                        foundation::ErrorCategory::Internal,
                        exception.what(),
                        request));
                } catch(...) {
                }
            } catch(...) {
                try {
                    response.postExecutionErrors.push_back(commandError(
                        "Command.PostAcceptanceIntegrationFailed",
                        foundation::ErrorCategory::Internal,
                        "Post-acceptance logging integration failed",
                        request));
                } catch(...) {
                }
            }
            return foundation::Result<CommandResponse>::success(std::move(response));
        }

        std::optional<state::RevisionPrecondition> expected;
        if(request.expectedRevision.has_value()) {
            expected = state::RevisionPrecondition {
                state::RevisionScope::Project, *request.expectedRevision};
        }
        const std::span<const state::RevisionPrecondition> preconditions = expected.has_value()
            ? std::span<const state::RevisionPrecondition> {&*expected, 1U}
            : std::span<const state::RevisionPrecondition> {};

        auto transactionId = kernel::TransactionId::create(
            "transaction." + std::string(request.requestId.value()));
        if(!transactionId.hasValue()) {
            logFailure(services.value(), request, &descriptor.version);
            return foundation::Result<CommandResponse>::failure(std::move(transactionId).error());
        }
        auto transaction = transactions.begin(
            std::move(transactionId).value(), *request.context.documentId, preconditions);
        if(!transaction.hasValue()) {
            logFailure(services.value(), request, &descriptor.version);
            return foundation::Result<CommandResponse>::failure(std::move(transaction).error());
        }
        if(transaction.value()->projectId() != *request.context.projectId) {
            static_cast<void>(transaction.value()->rollback());
            auto error = commandError(
                "Command.ProjectMismatch",
                foundation::ErrorCategory::Validation,
                "The command project does not own the target document",
                request);
            logFailure(services.value(), request, &descriptor.version);
            return foundation::Result<CommandResponse>::failure(std::move(error));
        }

        foundation::Result<foundation::Value> handled = [&]() {
            try {
                return entry.value().handler->execute(request, *transaction.value());
            } catch(const std::exception& exception) {
                return foundation::Result<foundation::Value>::failure(commandError(
                    "Command.HandlerFailed",
                    foundation::ErrorCategory::Internal,
                    "The command handler raised an exception",
                    request,
                    std::make_shared<const foundation::Error>(foundation::makeError(
                        "Command.HandlerException",
                        foundation::ErrorCategory::Internal,
                        exception.what()))));
            } catch(...) {
                return foundation::Result<foundation::Value>::failure(commandError(
                    "Command.HandlerFailed",
                    foundation::ErrorCategory::Internal,
                    "The command handler raised an exception",
                    request));
            }
        }();
        if(!handled.hasValue()) {
            static_cast<void>(transaction.value()->rollback());
            logFailure(services.value(), request, &descriptor.version);
            return foundation::Result<CommandResponse>::failure(std::move(handled).error());
        }

        auto resultValid = services.value().schemaValidator->validate(
            descriptor.result, handled.value());
        if(!resultValid.hasValue()) {
            static_cast<void>(transaction.value()->rollback());
            logFailure(services.value(), request, &descriptor.version);
            return foundation::Result<CommandResponse>::failure(std::move(resultValid).error());
        }

        if(durableLease != nullptr) {
            auto attached = transaction.value()->attachIdempotency(TransactionIdempotency {
                *request.idempotencyKey,
                durableLease->signature(),
                handled.value()});
            if(!attached) {
                static_cast<void>(transaction.value()->rollback());
                return foundation::Result<CommandResponse>::failure(
                    std::move(attached).error());
            }
        }

        auto committed = transaction.value()->commit();
        if(!committed.hasValue()) {
            logFailure(services.value(), request, &descriptor.version);
            return foundation::Result<CommandResponse>::failure(std::move(committed).error());
        }
        if(durableLease != nullptr) {
            durableLease->complete();
        }

        std::optional<CommandResponse> response;
        response.emplace(CommandResponse {
            std::move(handled).value(),
            std::move(committed).value(),
            std::nullopt,
            {},
            false,
            descriptor.version,
            descriptor.status});
        try {
            for(const auto& event : response->commit->events) {
                auto delivery = events.publish(event, request.correlationId, request.traceId);
                if(!delivery.hasValue()) {
                    response->postExecutionErrors.push_back(std::move(delivery).error());
                    continue;
                }
                for(auto& failure : delivery.value().failures) {
                    response->postExecutionErrors.push_back(std::move(failure.error));
                }
            }
            auto logged = services.value().logService->write(commandLog(
                request, observability::LogLevel::Info, "success", &descriptor.version));
            if(!logged.hasValue()) {
                response->postExecutionErrors.push_back(std::move(logged).error());
            }
        } catch(const std::exception& exception) {
            try {
                response->postExecutionErrors.push_back(commandError(
                    "Command.PostCommitIntegrationFailed",
                    foundation::ErrorCategory::Internal,
                    exception.what(),
                    request));
            } catch(...) {
            }
        } catch(...) {
            try {
                response->postExecutionErrors.push_back(commandError(
                    "Command.PostCommitIntegrationFailed",
                    foundation::ErrorCategory::Internal,
                    "Post-commit event or logging integration failed",
                    request));
            } catch(...) {
            }
        }
        return foundation::Result<CommandResponse>::success(std::move(*response));
    }

    void evictCompletedForCapacity()
    {
        while(idempotencyRecords.size() >= idempotencyCapacity
              && !completionOrder.empty()) {
            const auto key = completionOrder.front();
            completionOrder.pop_front();
            const auto record = idempotencyRecords.find(key);
            if(record != idempotencyRecords.end() && record->second->complete) {
                idempotencyRecords.erase(record);
            }
        }
    }

    CommandRegistry& registry;
    const state::DocumentStore& documents;
    EffectExecutor& effects;
    TransactionManager& transactions;
    CapabilityService& capabilities;
    messaging::EventBus& events;
    ExecutionServices& executionServices;
    TaskRuntime& tasks;
    persistence::PersistenceService& persistence;
    observability::ITraceService& traces;
    observability::IMetricsService& metrics;
    DocumentRuntime* documentRuntime;
    const std::size_t idempotencyCapacity;
    std::atomic_bool accepting{false};
    std::atomic_size_t activeExecutions{0U};
    mutable std::mutex idempotencyMutex;
    std::map<kernel::IdempotencyKey, std::shared_ptr<IdempotencyRecord>> idempotencyRecords;
    std::deque<kernel::IdempotencyKey> completionOrder;
};

CommandRuntime::CommandRuntime(
    CommandRegistry& registry,
    const state::DocumentStore& documents,
    EffectExecutor& effects,
    TransactionManager& transactions,
    CapabilityService& capabilities,
    messaging::EventBus& events,
    ExecutionServices& executionServices,
    TaskRuntime& tasks,
    persistence::PersistenceService& persistence,
    observability::ITraceService& traces,
    observability::IMetricsService& metrics,
    std::size_t idempotencyCapacity,
    DocumentRuntime* documentRuntime)
    : impl_(std::make_unique<Impl>(
          registry,
          documents,
          effects,
          transactions,
          capabilities,
          events,
          executionServices,
          tasks,
          persistence,
          traces,
          metrics,
          idempotencyCapacity,
          documentRuntime))
{
}

CommandRuntime::~CommandRuntime() = default;

foundation::Result<CommandResponse> CommandRuntime::execute(const CommandRequest& request)
{
    const auto startedAt = std::chrono::steady_clock::now();
    std::optional<kernel::SpanId> activeSpanId;
    std::unique_ptr<observability::ITraceSpan> span;
    startCommandSpan(impl_->traces, request, activeSpanId, span);

    auto observedResult = [&]() -> foundation::Result<CommandResponse> {
        if(!impl_->accepting.load(std::memory_order_acquire)) {
            return foundation::Result<CommandResponse>::failure(commandError(
                "Command.RuntimeNotReady",
                foundation::ErrorCategory::Conflict,
                "The command runtime is not accepting requests",
                request));
        }
        std::optional<DocumentActivityLease> documentActivity;
        if(impl_->documentRuntime != nullptr
           && request.context.documentId.has_value()) {
            auto admitted = impl_->documentRuntime->acquireActivity(
                *request.context.documentId, DocumentActivityKind::Command);
            if(!admitted) {
                return foundation::Result<CommandResponse>::failure(
                    std::move(admitted).error());
            }
            documentActivity.emplace(std::move(admitted).value());
        }
        ActiveExecution active(impl_->activeExecutions);

        if(!request.idempotencyKey.has_value()
           || impl_->isExternalEffectRequest(request)) {
            return impl_->executeSafe(request, activeSpanId);
        }

        std::shared_ptr<Impl::IdempotencyRecord> record;
        {
            std::unique_lock lock(impl_->idempotencyMutex);
            const auto existing = impl_->idempotencyRecords.find(*request.idempotencyKey);
            if(existing != impl_->idempotencyRecords.end()) {
                record = existing->second;
                if(record->signature != signatureOf(request)) {
                    return foundation::Result<CommandResponse>::failure(commandError(
                        "Command.IdempotencyKeyConflict",
                        foundation::ErrorCategory::Conflict,
                        "The idempotency key is already bound to a different request",
                        request));
                }
                if(!record->complete && record->owner == std::this_thread::get_id()) {
                    return foundation::Result<CommandResponse>::failure(commandError(
                        "Command.IdempotencyReentry",
                        foundation::ErrorCategory::Conflict,
                        "The same in-flight idempotency key cannot re-enter its execution thread",
                        request));
                }
                record->completed.wait(lock, [&]() { return record->complete; });
                if(record->abandoned || !record->result.has_value()) {
                    return foundation::Result<CommandResponse>::failure(commandError(
                        "Command.IdempotencyRecordLost",
                        foundation::ErrorCategory::Internal,
                        "The in-flight request completed without a reusable idempotency record",
                        request));
                }
                auto result = *record->result;
                if(result.hasValue()) {
                    result.value().replayed = true;
                }
                return result;
            }

            if(impl_->idempotencyCapacity == 0U) {
                return foundation::Result<CommandResponse>::failure(commandError(
                    "Command.IdempotencyDisabled",
                    foundation::ErrorCategory::Conflict,
                    "The command runtime has no idempotency record capacity",
                    request));
            }
            impl_->evictCompletedForCapacity();
            if(impl_->idempotencyRecords.size() >= impl_->idempotencyCapacity) {
                return foundation::Result<CommandResponse>::failure(commandError(
                    "Command.IdempotencyCapacityExhausted",
                    foundation::ErrorCategory::Conflict,
                    "All bounded idempotency records are currently in flight",
                    request));
            }
            record = std::make_shared<Impl::IdempotencyRecord>(
                signatureOf(request), std::this_thread::get_id());
            impl_->idempotencyRecords.emplace(*request.idempotencyKey, record);
        }

        auto result = impl_->executeSafe(request, activeSpanId);
        std::optional<foundation::Result<CommandResponse>> cached;
        try {
            cached.emplace(result);
        } catch(...) {
            {
                std::lock_guard lock(impl_->idempotencyMutex);
                record->abandoned = true;
                record->complete = true;
                impl_->idempotencyRecords.erase(*request.idempotencyKey);
            }
            record->completed.notify_all();
            return result;
        }
        {
            std::lock_guard lock(impl_->idempotencyMutex);
            record->result = std::move(cached);
            record->complete = true;
            try {
                impl_->completionOrder.push_back(*request.idempotencyKey);
            } catch(...) {
            }
        }
        record->completed.notify_all();
        return result;
    }();

    const auto succeeded = observedResult.hasValue();
    if(span != nullptr) {
        span->end(
            succeeded ? observability::TraceStatus::Succeeded
                      : observability::TraceStatus::Failed,
            succeeded ? std::nullopt
                      : std::optional<foundation::Error> {observedResult.error()});
    }
    recordCommandMetrics(
        impl_->metrics, succeeded, std::chrono::steady_clock::now() - startedAt);
    return observedResult;
}

std::size_t CommandRuntime::activeExecutionCount() const noexcept
{
    return impl_->activeExecutions.load(std::memory_order_acquire);
}

std::size_t CommandRuntime::idempotencyRecordCount() const
{
    std::lock_guard lock(impl_->idempotencyMutex);
    return impl_->idempotencyRecords.size();
}

bool CommandRuntime::accepting() const noexcept
{
    return impl_->accepting.load(std::memory_order_acquire);
}

void CommandRuntime::start() noexcept
{
    impl_->accepting.store(true, std::memory_order_release);
}

void CommandRuntime::stop() noexcept
{
    impl_->accepting.store(false, std::memory_order_release);
}

} // namespace lasercnc::runtime
