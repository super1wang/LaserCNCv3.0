#include <lasercnc/runtime/scheduler.hpp>

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/persistence/persistence_service.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace lasercnc::runtime {
namespace {

foundation::Error taskError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const kernel::TaskId& taskId)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {foundation::Value::Object {
            {"taskId", foundation::Value {std::string(taskId.value())}},
        }});
}

foundation::Error schedulerError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message)
{
    return foundation::makeError(code, category, message);
}

foundation::Result<kernel::SpanId> taskSpanId(const kernel::TaskId& taskId)
{
    static std::atomic_ullong sequence {0U};
    return kernel::SpanId::create(
        "span.task." + std::string(taskId.value()) + "."
        + std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed)));
}

const char* taskStateLabel(TaskState state) noexcept
{
    switch(state) {
    case TaskState::Pending: return "pending";
    case TaskState::Ready: return "ready";
    case TaskState::Running: return "running";
    case TaskState::Succeeded: return "succeeded";
    case TaskState::Failed: return "failed";
    case TaskState::CancelRequested: return "cancel_requested";
    case TaskState::Cancelled: return "cancelled";
    case TaskState::Stale: return "stale";
    }
    return "unknown";
}

observability::TraceStatus traceStatus(TaskState state) noexcept
{
    switch(state) {
    case TaskState::Succeeded: return observability::TraceStatus::Succeeded;
    case TaskState::Cancelled: return observability::TraceStatus::Cancelled;
    case TaskState::Stale: return observability::TraceStatus::Stale;
    default: return observability::TraceStatus::Failed;
    }
}

void recordTaskMetrics(
    observability::IMetricsService& metrics,
    TaskState state,
    std::chrono::steady_clock::duration elapsed) noexcept
{
    try {
        const observability::MetricLabels labels {{"outcome", taskStateLabel(state)}};
        auto completedMetric = kernel::MetricName::create("kernel.task.completed");
        if(completedMetric) {
            static_cast<void>(metrics.addCounter(
                std::move(completedMetric).value(), 1.0, labels));
        }
        auto durationMetric = kernel::MetricName::create("kernel.task.duration_ms");
        if(durationMetric) {
            const auto duration = std::chrono::duration<double, std::milli>(elapsed).count();
            static_cast<void>(metrics.observeHistogram(
                std::move(durationMetric).value(), duration, labels));
        }
    } catch(...) {
    }
}

} // namespace

struct Scheduler::Outcome final {
    std::mutex mutex;
    std::optional<foundation::Value> result;
    std::optional<foundation::Error> error;
    std::unique_ptr<observability::ITraceSpan> span;
    std::chrono::steady_clock::time_point startedAt{std::chrono::steady_clock::now()};
};

struct Scheduler::Core final {
    struct Record final {
        TaskDescriptor descriptor;
        std::shared_ptr<ITaskHandler> handler;
        TaskRequest request;
        TaskState state{TaskState::Pending};
        double progress{0.0};
        std::string progressMessage;
        std::optional<foundation::Value> result;
        std::optional<foundation::Error> error;
        std::shared_ptr<CancellationToken::State> cancellation;
        std::optional<state::Document> document;
        std::function<bool()> sourceIsStale;
        std::size_t sequence{0U};
        bool resourcesHeld{false};
        bool durablyAccepted{true};
        bool completionReady{true};
    };

    static TaskSnapshot snapshotOf(const Record& record)
    {
        return TaskSnapshot {
            record.request.taskId,
            record.request.task,
            record.state,
            record.progress,
            record.progressMessage,
            record.request.traceId,
            record.document.has_value()
                ? std::optional<state::RevisionSet> {record.document->revisions()}
                : std::nullopt,
            record.result,
            record.error};
    }

    Core(
        ResourceManager& resourceManager,
        persistence::PersistenceService* persistenceService,
        observability::ITraceService& traceService,
        observability::IMetricsService& metricsService)
        : resources(&resourceManager),
          persistence(persistenceService),
          traces(&traceService),
          metrics(&metricsService)
    {
    }

    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    std::map<kernel::TaskId, Record> records;
    std::vector<foundation::Error> persistenceFailures;
    ResourceManager* resources;
    persistence::PersistenceService* persistence;
    observability::ITraceService* traces;
    observability::IMetricsService* metrics;
    platform::ITaskExecutor* executor{nullptr};
    std::size_t nextSequence{0U};
    std::size_t runningCount{0U};
    bool configured{false};
    bool started{false};
    bool accepting{false};
    bool dispatching{false};
};

Scheduler::Scheduler(
    ResourceManager& resources,
    observability::ITraceService& traces,
    observability::IMetricsService& metrics)
    : core_(std::make_shared<Core>(resources, nullptr, traces, metrics))
{
}

Scheduler::Scheduler(
    ResourceManager& resources,
    persistence::PersistenceService& persistence,
    observability::ITraceService& traces,
    observability::IMetricsService& metrics)
    : core_(std::make_shared<Core>(resources, &persistence, traces, metrics))
{
}

Scheduler::~Scheduler() = default;

foundation::Result<void> Scheduler::configureExecutor(platform::ITaskExecutor& executor)
{
    std::lock_guard lock(core_->mutex);
    if(core_->started || core_->configured) {
        return foundation::Result<void>::failure(schedulerError(
            "Task.ExecutorAlreadyConfigured",
            foundation::ErrorCategory::Conflict,
            "The scheduler executor can only be configured once before start"));
    }
    if(executor.concurrency() == 0U) {
        return foundation::Result<void>::failure(schedulerError(
            "Task.ExecutorHasNoWorkers",
            foundation::ErrorCategory::Validation,
            "The scheduler executor must expose at least one worker"));
    }
    core_->executor = &executor;
    core_->configured = true;
    return foundation::Result<void>::success();
}

foundation::Result<void> Scheduler::start()
{
    {
        std::lock_guard lock(core_->mutex);
        if(!core_->configured || core_->executor == nullptr) {
            return foundation::Result<void>::failure(schedulerError(
                "Task.ExecutorNotConfigured",
                foundation::ErrorCategory::Conflict,
                "The scheduler requires an executor before start"));
        }
        if(core_->started) {
            return foundation::Result<void>::failure(schedulerError(
                "Task.SchedulerAlreadyStarted",
                foundation::ErrorCategory::Conflict,
                "The scheduler can only be started once"));
        }
        core_->started = true;
        core_->accepting = true;
    }
    core_->resources->freeze();
    pump(core_);
    return foundation::Result<void>::success();
}

foundation::Result<void> Scheduler::schedule(
    TaskDescriptor descriptor,
    std::shared_ptr<ITaskHandler> handler,
    TaskRequest request,
    std::optional<state::Document> document,
    std::function<bool()> sourceIsStale,
    bool activateImmediately)
{
    {
        std::lock_guard lock(core_->mutex);
        if(!core_->started || !core_->accepting) {
            return foundation::Result<void>::failure(taskError(
                "Task.RuntimeNotAccepting",
                foundation::ErrorCategory::Conflict,
                "The task runtime is not accepting new work",
                request.taskId));
        }
        if(request.task != descriptor.name) {
            return foundation::Result<void>::failure(taskError(
                "Task.DescriptorMismatch",
                foundation::ErrorCategory::Validation,
                "The request task name does not match its descriptor",
                request.taskId));
        }
        if(core_->records.contains(request.taskId)) {
            return foundation::Result<void>::failure(taskError(
                "Task.IdAlreadyExists",
                foundation::ErrorCategory::Conflict,
                "A task with the same stable id already exists",
                request.taskId));
        }
        std::set<kernel::TaskId> uniqueDependencies;
        for(const auto& dependency : request.dependencies) {
            if(dependency == request.taskId) {
                return foundation::Result<void>::failure(taskError(
                    "Task.SelfDependency",
                    foundation::ErrorCategory::Validation,
                    "A task cannot depend on itself",
                    request.taskId));
            }
            if(!uniqueDependencies.insert(dependency).second) {
                return foundation::Result<void>::failure(taskError(
                    "Task.DuplicateDependency",
                    foundation::ErrorCategory::Validation,
                    "A task dependency can only be declared once",
                    request.taskId));
            }
            if(!core_->records.contains(dependency)) {
                return foundation::Result<void>::failure(taskError(
                    "Task.DependencyNotFound",
                    foundation::ErrorCategory::NotFound,
                    "A declared task dependency does not exist",
                    request.taskId));
            }
        }

        auto cancellation = std::make_shared<CancellationToken::State>();
        cancellation->deadline = request.deadline;
        const auto sequence = core_->nextSequence++;
        const auto taskId = request.taskId;
        core_->records.emplace(
            taskId,
            Core::Record {
                std::move(descriptor),
                std::move(handler),
                std::move(request),
                TaskState::Pending,
                0.0,
                {},
                std::nullopt,
                std::nullopt,
                std::move(cancellation),
                std::move(document),
                std::move(sourceIsStale),
                sequence,
                false,
                activateImmediately});
    }
    core_->changed.notify_all();
    if(activateImmediately) {
        pump(core_);
    }
    return foundation::Result<void>::success();
}

foundation::Result<void> Scheduler::activate(const kernel::TaskId& taskId)
{
    {
        std::lock_guard lock(core_->mutex);
        const auto found = core_->records.find(taskId);
        if(found == core_->records.end()) {
            return foundation::Result<void>::failure(taskError(
                "Task.PreparedRecordMissing",
                foundation::ErrorCategory::Internal,
                "A durably accepted task has no prepared scheduler record",
                taskId));
        }
        found->second.durablyAccepted = true;
    }
    core_->changed.notify_all();
    pump(core_);
    return foundation::Result<void>::success();
}

void Scheduler::discardPrepared(const kernel::TaskId& taskId) noexcept
{
    try {
        std::lock_guard lock(core_->mutex);
        const auto found = core_->records.find(taskId);
        if(found != core_->records.end() && !found->second.durablyAccepted
           && found->second.state == TaskState::Pending) {
            core_->records.erase(found);
        }
    } catch(...) {
    }
    core_->changed.notify_all();
}

foundation::Result<void> Scheduler::requestCancel(const kernel::TaskId& taskId)
{
    std::optional<TaskSnapshot> terminal;
    {
        std::lock_guard lock(core_->mutex);
        const auto found = core_->records.find(taskId);
        if(found == core_->records.end()) {
            return foundation::Result<void>::failure(taskError(
                "Task.IdNotFound",
                foundation::ErrorCategory::NotFound,
                "The task id does not exist",
                taskId));
        }
        auto& record = found->second;
        if(isTerminal(record.state)) {
            return foundation::Result<void>::success();
        }
        record.cancellation->requested.store(true, std::memory_order_release);
        // Repeated cancellation must not finish work that still owns a worker/resources.
        // 中文翻译：重复取消不能提前终结仍持有工作线程和资源的任务。
        if(record.state == TaskState::Running || record.state == TaskState::CancelRequested) {
            record.state = TaskState::CancelRequested;
        } else {
            record.state = TaskState::Cancelled;
            record.completionReady = false;
            record.error = taskError(
                "Task.Cancelled",
                foundation::ErrorCategory::Cancellation,
                "The task was cancelled before execution",
                taskId);
            terminal = Core::snapshotOf(record);
        }
    }
    core_->changed.notify_all();
    if(terminal.has_value()) {
        persistTerminal(core_, *terminal);
    }
    pump(core_);
    return foundation::Result<void>::success();
}

foundation::Result<TaskSnapshot> Scheduler::snapshot(const kernel::TaskId& taskId) const
{
    pump(core_);
    std::lock_guard lock(core_->mutex);
    const auto found = core_->records.find(taskId);
    if(found == core_->records.end()) {
        return foundation::Result<TaskSnapshot>::failure(taskError(
            "Task.IdNotFound",
            foundation::ErrorCategory::NotFound,
            "The task id does not exist",
            taskId));
    }
    const auto& record = found->second;
    return foundation::Result<TaskSnapshot>::success(Core::snapshotOf(record));
}

foundation::Result<TaskSnapshot> Scheduler::wait(
    const kernel::TaskId& taskId,
    std::chrono::milliseconds timeout) const
{
    if(timeout < std::chrono::milliseconds::zero()) {
        return foundation::Result<TaskSnapshot>::failure(taskError(
            "Task.InvalidWaitTimeout",
            foundation::ErrorCategory::Validation,
            "Task wait timeout cannot be negative",
            taskId));
    }
    const auto end = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock(core_->mutex);
    while(true) {
        const auto found = core_->records.find(taskId);
        if(found == core_->records.end()) {
            return foundation::Result<TaskSnapshot>::failure(taskError(
                "Task.IdNotFound",
                foundation::ErrorCategory::NotFound,
                "The task id does not exist",
                taskId));
        }
        if(isTerminal(found->second.state) && found->second.completionReady) {
            const auto& record = found->second;
            return foundation::Result<TaskSnapshot>::success(Core::snapshotOf(record));
        }
        if(std::chrono::steady_clock::now() >= end) {
            return foundation::Result<TaskSnapshot>::failure(taskError(
                "Task.WaitTimeout",
                foundation::ErrorCategory::Timeout,
                "The task did not reach a terminal state before the wait timeout",
                taskId));
        }
        core_->changed.wait_until(lock, std::min(end, std::chrono::steady_clock::now() + std::chrono::milliseconds(10)));
        lock.unlock();
        pump(core_);
        lock.lock();
    }
}

foundation::Result<void> Scheduler::shutdown(std::chrono::milliseconds timeout)
{
    if(timeout < std::chrono::milliseconds::zero()) {
        return foundation::Result<void>::failure(schedulerError(
            "Task.InvalidShutdownTimeout",
            foundation::ErrorCategory::Validation,
            "Scheduler shutdown timeout cannot be negative"));
    }
    const auto end = std::chrono::steady_clock::now() + timeout;
    std::vector<TaskSnapshot> terminal;
    {
        std::lock_guard lock(core_->mutex);
        if(!core_->started) {
            return foundation::Result<void>::success();
        }
        core_->accepting = false;
        for(auto& [taskId, record] : core_->records) {
            if(isTerminal(record.state)) {
                continue;
            }
            record.cancellation->requested.store(true, std::memory_order_release);
            if(record.state == TaskState::Running || record.state == TaskState::CancelRequested) {
                record.state = TaskState::CancelRequested;
            } else {
                record.state = TaskState::Cancelled;
                record.completionReady = false;
                record.error = taskError(
                    "Task.CancelledByShutdown",
                    foundation::ErrorCategory::Cancellation,
                    "The task was cancelled during scheduler shutdown",
                    taskId);
                terminal.push_back(Core::snapshotOf(record));
            }
        }
    }
    core_->changed.notify_all();
    for(const auto& snapshot : terminal) {
        persistTerminal(core_, snapshot);
    }

    std::unique_lock lock(core_->mutex);
    if(!core_->changed.wait_until(lock, end, [core = core_]() { return core->runningCount == 0U; })) {
        return foundation::Result<void>::failure(schedulerError(
            "Task.ShutdownTimeout",
            foundation::ErrorCategory::Timeout,
            "Running tasks did not cooperatively stop before the shutdown deadline"));
    }
    core_->started = false;
    auto* executor = core_->executor;
    lock.unlock();
    return executor->shutdown();
}

std::size_t Scheduler::activeTaskCount() const
{
    std::lock_guard lock(core_->mutex);
    return static_cast<std::size_t>(std::count_if(
        core_->records.begin(),
        core_->records.end(),
        [](const auto& entry) { return !isTerminal(entry.second.state) || !entry.second.completionReady; }));
}

std::size_t Scheduler::activeTaskCount(
    const kernel::DocumentId& documentId) const
{
    std::lock_guard lock(core_->mutex);
    std::size_t count = 0U;
    for(const auto& [unusedTaskId, record] : core_->records) {
        static_cast<void>(unusedTaskId);
        if(record.request.documentId.has_value()
           && *record.request.documentId == documentId
           && (!isTerminal(record.state) || !record.completionReady)) {
            ++count;
        }
    }
    return count;
}

std::size_t Scheduler::activeTaskCount(const kernel::ProjectId& projectId) const
{
    std::lock_guard lock(core_->mutex);
    std::size_t count = 0U;
    for(const auto& [unusedTaskId, record] : core_->records) {
        static_cast<void>(unusedTaskId);
        if(record.request.projectId == projectId
           && (!isTerminal(record.state) || !record.completionReady)) {
            ++count;
        }
    }
    return count;
}

std::vector<foundation::Error> Scheduler::persistenceFailures() const
{
    std::lock_guard lock(core_->mutex);
    return core_->persistenceFailures;
}

void Scheduler::pump(const std::shared_ptr<Core>& core)
{
    {
        std::lock_guard lock(core->mutex);
        if(!core->started || core->dispatching) {
            return;
        }
        core->dispatching = true;
    }

    while(true) {
        std::optional<kernel::TaskId> selected;
        std::shared_ptr<ITaskHandler> handler;
        std::optional<TaskRequest> request;
        std::optional<state::Document> document;
        CancellationToken token;
        bool foundCandidate = false;
        std::vector<TaskSnapshot> terminal;

        {
            std::lock_guard lock(core->mutex);
            const auto now = std::chrono::steady_clock::now();
            for(auto& [taskId, record] : core->records) {
                if(!record.durablyAccepted
                   || (record.state != TaskState::Pending
                       && record.state != TaskState::Ready)) {
                    continue;
                }
                if(record.request.deadline.has_value() && now >= *record.request.deadline) {
                    record.cancellation->requested.store(true, std::memory_order_release);
                    record.state = TaskState::Cancelled;
                    record.completionReady = false;
                    record.error = taskError(
                        "Task.DeadlineExceeded",
                        foundation::ErrorCategory::Timeout,
                        "The task deadline elapsed before execution",
                        taskId);
                    terminal.push_back(Core::snapshotOf(record));
                    continue;
                }
                bool waiting = false;
                bool stale = false;
                for(const auto& dependencyId : record.request.dependencies) {
                    const auto& dependency = core->records.at(dependencyId);
                    if(!isTerminal(dependency.state)) {
                        waiting = true;
                        break;
                    }
                    if(dependency.state != TaskState::Succeeded) {
                        stale = true;
                        break;
                    }
                }
                if(stale) {
                    record.state = TaskState::Stale;
                    record.completionReady = false;
                    record.error = taskError(
                        "Task.DependencyDidNotSucceed",
                        foundation::ErrorCategory::Conflict,
                        "A task dependency did not succeed",
                        taskId);
                    terminal.push_back(Core::snapshotOf(record));
                } else {
                    record.state = waiting ? TaskState::Pending : TaskState::Ready;
                }
            }

            if(core->runningCount < core->executor->concurrency()) {
                std::vector<std::pair<const kernel::TaskId*, Core::Record*>> candidates;
                for(auto& [taskId, record] : core->records) {
                    if(record.state == TaskState::Ready) {
                        candidates.emplace_back(&taskId, &record);
                    }
                }
                std::sort(
                    candidates.begin(),
                    candidates.end(),
                    [](const auto& left, const auto& right) {
                        return left.second->request.priority > right.second->request.priority
                            || (left.second->request.priority == right.second->request.priority
                                && left.second->sequence < right.second->sequence);
                    });
                for(auto [candidateId, candidate] : candidates) {
                    auto acquired = core->resources->tryAcquire(candidate->request.resources);
                    if(!acquired) {
                        candidate->state = TaskState::Failed;
                        candidate->completionReady = false;
                        candidate->error = std::move(acquired).error();
                        terminal.push_back(Core::snapshotOf(*candidate));
                    } else if(acquired.value()) {
                        candidate->resourcesHeld = true;
                        candidate->state = TaskState::Running;
                        ++core->runningCount;
                        selected = *candidateId;
                        handler = candidate->handler;
                        request = candidate->request;
                        document = candidate->document;
                        token = CancellationToken(candidate->cancellation);
                        foundCandidate = true;
                        break;
                    }
                }
            }

            if(!foundCandidate) {
                core->dispatching = false;
                core->changed.notify_all();
            }
        }

        for(const auto& snapshot : terminal) {
            persistTerminal(core, snapshot);
        }
        if(!foundCandidate) {
            return;
        }

        const auto taskId = *selected;
        const auto taskRequest = *request;
        auto outcome = std::make_shared<Outcome>();
        std::optional<kernel::SpanId> activeSpanId;
        try {
            auto createdSpanId = taskSpanId(taskId);
            if(createdSpanId) {
                const auto spanId = createdSpanId.value();
                auto span = core->traces->startSpan(observability::TraceSpanStart {
                    taskRequest.traceId,
                    spanId,
                    taskRequest.parentSpanId,
                    "task.execute",
                    foundation::Value::Object {
                        {"task", foundation::Value {std::string(taskRequest.task.value())}},
                    }});
                if(span && span.value() != nullptr) {
                    activeSpanId = spanId;
                    outcome->span = std::move(span).value();
                }
            }
        } catch(...) {
        }
        const auto weakCore = std::weak_ptr<Core>(core);
        ProgressReporter reporter(ProgressReporter::Callback {
            [weakCore, taskId](double completed, std::string message) {
                if(!std::isfinite(completed) || completed < 0.0 || completed > 1.0) {
                    return foundation::Result<void>::failure(taskError(
                        "Task.InvalidProgress",
                        foundation::ErrorCategory::Validation,
                        "Task progress must be finite and within [0, 1]",
                        taskId));
                }
                const auto locked = weakCore.lock();
                if(locked == nullptr) {
                    return foundation::Result<void>::failure(taskError(
                        "Task.ProgressReporterExpired",
                        foundation::ErrorCategory::Conflict,
                        "The task progress reporter has expired",
                        taskId));
                }
                std::lock_guard lock(locked->mutex);
                const auto found = locked->records.find(taskId);
                if(found == locked->records.end() || isTerminal(found->second.state)) {
                    return foundation::Result<void>::failure(taskError(
                        "Task.ProgressAfterCompletion",
                        foundation::ErrorCategory::Conflict,
                        "Progress cannot be reported after task completion",
                        taskId));
                }
                if(completed < found->second.progress) {
                    return foundation::Result<void>::failure(taskError(
                        "Task.ProgressRegression",
                        foundation::ErrorCategory::Conflict,
                        "Task progress must be monotonic",
                        taskId));
                }
                found->second.progress = completed;
                found->second.progressMessage = std::move(message);
                locked->changed.notify_all();
                return foundation::Result<void>::success();
            }});
        TaskContext context {
            token,
            std::move(reporter),
            taskRequest.traceId,
            activeSpanId,
            ResourceContext {taskRequest.resources},
            std::move(document)};

        auto submitted = [&]() -> foundation::Result<void> {
            try {
                return core->executor->submit(
                    [handler = std::move(handler), taskRequest, context, outcome]() mutable {
                        auto result = handler->execute(taskRequest, context);
                        std::lock_guard lock(outcome->mutex);
                        if(result) {
                            outcome->result = std::move(result).value();
                            return foundation::Result<void>::success();
                        }
                        outcome->error = std::move(result).error();
                        return foundation::Result<void>::failure(*outcome->error);
                    },
                    [core, taskId, outcome](foundation::Result<void> result) mutable {
                        finish(core, taskId, std::move(result), outcome);
                    });
            } catch(...) {
                return foundation::Result<void>::failure(taskError(
                    "Task.ExecutorSubmitFailed", foundation::ErrorCategory::Infrastructure,
                    "The executor raised an exception before accepting task work", taskId));
            }
        }();
        if(!submitted) {
            finish(core, taskId, std::move(submitted), outcome);
        }
    }
}

void Scheduler::finish(
    const std::shared_ptr<Core>& core,
    const kernel::TaskId& taskId,
    foundation::Result<void> executionResult,
    const std::shared_ptr<Outcome>& outcome)
{
    std::function<bool()> sourceIsStale;
    std::optional<TaskSnapshot> terminal;
    {
        std::lock_guard lock(core->mutex);
        const auto found = core->records.find(taskId);
        if(found != core->records.end()
           && (found->second.state == TaskState::Running
               || found->second.state == TaskState::CancelRequested)) {
            sourceIsStale = found->second.sourceIsStale;
        } else {
            return;
        }
    }
    const bool staleSource = sourceIsStale && sourceIsStale();
    TaskState finalState{TaskState::Failed};
    std::optional<foundation::Error> finalError;
    {
        std::lock_guard lock(core->mutex);
        const auto found = core->records.find(taskId);
        if(found == core->records.end()) {
            return;
        }
        auto& record = found->second;
        if(record.state != TaskState::Running && record.state != TaskState::CancelRequested) {
            return;
        }
        if(record.resourcesHeld) {
            core->resources->release(record.request.resources);
            record.resourcesHeld = false;
        }
        if(core->runningCount != 0U) {
            --core->runningCount;
        }

        const bool deadlineExceeded = record.request.deadline.has_value()
            && std::chrono::steady_clock::now() >= *record.request.deadline;
        if(record.cancellation->requested.load(std::memory_order_acquire) || deadlineExceeded) {
            record.state = TaskState::Cancelled;
            record.error = taskError(
                deadlineExceeded ? "Task.DeadlineExceeded" : "Task.Cancelled",
                deadlineExceeded ? foundation::ErrorCategory::Timeout
                                 : foundation::ErrorCategory::Cancellation,
                deadlineExceeded ? "The task exceeded its deadline"
                                 : "The task cooperatively stopped after cancellation",
                taskId);
        } else if(!executionResult) {
            record.state = TaskState::Failed;
            std::lock_guard outcomeLock(outcome->mutex);
            record.error = outcome->error.has_value() ? outcome->error
                                                      : std::optional<foundation::Error> {
                                                            std::move(executionResult).error()};
        } else if(staleSource) {
            record.state = TaskState::Stale;
            record.error = taskError(
                "Task.SourceRevisionChanged",
                foundation::ErrorCategory::Conflict,
                "The task source document changed while background work was running",
                taskId);
        } else {
            record.state = TaskState::Succeeded;
            record.progress = 1.0;
            std::lock_guard outcomeLock(outcome->mutex);
            record.result = outcome->result;
        }
        finalState = record.state;
        finalError = record.error;
        record.completionReady = false;
        terminal = Core::snapshotOf(record);
    }
    if(outcome->span != nullptr) {
        outcome->span->end(traceStatus(finalState), finalError);
    }
    recordTaskMetrics(
        *core->metrics, finalState, std::chrono::steady_clock::now() - outcome->startedAt);
    if(terminal.has_value()) {
        persistTerminal(core, *terminal);
    }
    core->changed.notify_all();
    pump(core);
}

void Scheduler::persistTerminal(
    const std::shared_ptr<Core>& core,
    const TaskSnapshot& snapshot) noexcept
{
    std::optional<foundation::Error> failure;
    if(core->persistence != nullptr && core->persistence->configured()) {
        try {
            auto persisted = core->persistence->recordTaskTerminal(snapshot);
            if(!persisted) {
                failure = std::move(persisted).error();
            }
        } catch(const std::exception& exception) {
            failure = taskError(
                "Task.PersistenceThrew",
                foundation::ErrorCategory::Internal,
                exception.what(),
                snapshot.taskId);
        } catch(...) {
            failure = taskError(
                "Task.PersistenceThrew",
                foundation::ErrorCategory::Internal,
                "Task terminal persistence raised an unknown exception",
                snapshot.taskId);
        }
    }
    {
        std::lock_guard lock(core->mutex);
        constexpr std::size_t failureCapacity = 256U;
        if(failure.has_value()) {
            if(core->persistenceFailures.size() >= failureCapacity) {
                core->persistenceFailures.erase(core->persistenceFailures.begin());
            }
            core->persistenceFailures.push_back(std::move(*failure));
        }
        const auto found = core->records.find(snapshot.taskId);
        if(found != core->records.end() && isTerminal(found->second.state)) {
            found->second.completionReady = true;
        }
    }
    core->changed.notify_all();
}

} // namespace lasercnc::runtime
