#include <lasercnc/runtime/command_runtime.hpp>

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/messaging/event_bus.hpp>
#include <lasercnc/observability/log_service.hpp>
#include <lasercnc/runtime/capability_service.hpp>
#include <lasercnc/runtime/command_registry.hpp>
#include <lasercnc/runtime/execution_services.hpp>
#include <lasercnc/runtime/transaction_manager.hpp>
#include <lasercnc/runtime/task_runtime.hpp>

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
        }},
        foundation::Severity::Error,
        std::move(cause));
}

struct RequestSignature final {
    kernel::SessionId sessionId;
    kernel::ProjectId projectId;
    kernel::DocumentId documentId;
    kernel::CommandName command;
    foundation::Value arguments;
    std::optional<state::Revision> expectedRevision;

    friend bool operator==(const RequestSignature&, const RequestSignature&) = default;
};

RequestSignature signatureOf(const CommandRequest& request)
{
    return RequestSignature {
        request.sessionId,
        request.projectId,
        request.documentId,
        request.command,
        request.arguments,
        request.expectedRevision};
}

observability::LogRecord commandLog(
    const CommandRequest& request,
    observability::LogLevel level,
    const char* outcome,
    const foundation::Version* version = nullptr)
{
    foundation::Value::Object data {
        {"command", foundation::Value {std::string(request.command.value())}},
        {"outcome", foundation::Value {outcome}},
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
            std::string(request.sessionId.value()),
            std::string(request.projectId.value()),
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
        TransactionManager& transactionManager,
        CapabilityService& capabilityService,
        messaging::EventBus& eventBus,
        ExecutionServices& services,
        TaskRuntime& taskRuntime,
        std::size_t capacity)
        : registry(commandRegistry),
          transactions(transactionManager),
          capabilities(capabilityService),
          events(eventBus),
          executionServices(services),
          tasks(taskRuntime),
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

    foundation::Result<CommandResponse> executeSafe(const CommandRequest& request)
    {
        try {
            return executeOnce(request);
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

    foundation::Result<CommandResponse> executeOnce(const CommandRequest& request)
    {
        auto entry = registry.resolve(request.command);
        if(!entry.hasValue()) {
            return foundation::Result<CommandResponse>::failure(std::move(entry).error());
        }

        auto services = executionServices.snapshot();
        if(!services.hasValue()) {
            return foundation::Result<CommandResponse>::failure(std::move(services).error());
        }
        const auto& descriptor = entry.value().descriptor;

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
        auto authorized = capabilities.authorize(request.sessionId, descriptor.capability);
        if(!authorized.hasValue()) {
            logFailure(services.value(), request, &descriptor.version);
            return foundation::Result<CommandResponse>::failure(std::move(authorized).error());
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
            plan.task.projectId = request.projectId;
            plan.task.documentId = request.documentId;
            plan.task.expectedProjectRevision = request.expectedRevision;
            const auto taskId = plan.task.taskId;
            CommandResponse response {
                std::move(plan.acceptance), std::nullopt, taskId, {}, false};
            auto submitted = tasks.submit(std::move(plan.task));
            if(!submitted) {
                logFailure(services.value(), request, &descriptor.version);
                return foundation::Result<CommandResponse>::failure(std::move(submitted).error());
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
            std::move(transactionId).value(), request.documentId, preconditions);
        if(!transaction.hasValue()) {
            logFailure(services.value(), request, &descriptor.version);
            return foundation::Result<CommandResponse>::failure(std::move(transaction).error());
        }
        if(transaction.value()->projectId() != request.projectId) {
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

        auto committed = transaction.value()->commit();
        if(!committed.hasValue()) {
            logFailure(services.value(), request, &descriptor.version);
            return foundation::Result<CommandResponse>::failure(std::move(committed).error());
        }

        std::optional<CommandResponse> response;
        response.emplace(CommandResponse {
            std::move(handled).value(), std::move(committed).value(), std::nullopt, {}, false});
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
    TransactionManager& transactions;
    CapabilityService& capabilities;
    messaging::EventBus& events;
    ExecutionServices& executionServices;
    TaskRuntime& tasks;
    const std::size_t idempotencyCapacity;
    std::atomic_bool accepting{false};
    std::atomic_size_t activeExecutions{0U};
    mutable std::mutex idempotencyMutex;
    std::map<kernel::IdempotencyKey, std::shared_ptr<IdempotencyRecord>> idempotencyRecords;
    std::deque<kernel::IdempotencyKey> completionOrder;
};

CommandRuntime::CommandRuntime(
    CommandRegistry& registry,
    TransactionManager& transactions,
    CapabilityService& capabilities,
    messaging::EventBus& events,
    ExecutionServices& executionServices,
    TaskRuntime& tasks,
    std::size_t idempotencyCapacity)
    : impl_(std::make_unique<Impl>(
          registry,
          transactions,
          capabilities,
          events,
          executionServices,
          tasks,
          idempotencyCapacity))
{
}

CommandRuntime::~CommandRuntime() = default;

foundation::Result<CommandResponse> CommandRuntime::execute(const CommandRequest& request)
{
    if(!impl_->accepting.load(std::memory_order_acquire)) {
        return foundation::Result<CommandResponse>::failure(commandError(
            "Command.RuntimeNotReady",
            foundation::ErrorCategory::Conflict,
            "The command runtime is not accepting requests",
            request));
    }
    ActiveExecution active(impl_->activeExecutions);

    if(!request.idempotencyKey.has_value()) {
        return impl_->executeSafe(request);
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

    auto result = impl_->executeSafe(request);
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
