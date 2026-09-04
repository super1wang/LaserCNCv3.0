#include <lasercnc/runtime/project_runtime.hpp>
#include "../../kernel/execution_admission.hpp"

#include <lasercnc/persistence/persistence_service.hpp>
#include <lasercnc/runtime/document_runtime.hpp>

#include <exception>
#include <string>

namespace lasercnc::runtime {
namespace {
foundation::Error projectError(const char* code, const kernel::ProjectId& id)
{
    return foundation::makeError(code, foundation::ErrorCategory::Conflict,
        "The project lifecycle operation could not be admitted",
        foundation::Value{foundation::Value::Object{{"projectId", foundation::Value{std::string(id.value())}}}});
}
}

struct ProjectActivityLease::Token final {
    explicit Token(kernel::ProjectId value) : projectId(std::move(value)) {}
    ~Token() { if(owner != nullptr) { owner->releaseActivity(projectId); } }
    kernel::ProjectId projectId;
    const ProjectRuntime* owner{nullptr};
};

bool ProjectActivityLease::matches(const kernel::ProjectId& id) const noexcept
{
    return token_ == nullptr || token_->projectId == id;
}

const char* projectLifecycleStateName(ProjectLifecycleState state) noexcept
{
    switch(state) {
    case ProjectLifecycleState::Closed: return "closed";
    case ProjectLifecycleState::Opening: return "opening";
    case ProjectLifecycleState::Open: return "open";
    case ProjectLifecycleState::Closing: return "closing";
    case ProjectLifecycleState::Failed: return "failed";
    }
    return "unknown";
}

ProjectRuntime::ProjectRuntime(persistence::PersistenceService& persistence) noexcept
    : persistence_(persistence) {}

foundation::Result<void> ProjectRuntime::persist(const kernel::ProjectId& id, ProjectLifecycleState state)
{
    if(!persistence_.configured()) { return foundation::Result<void>::success(); }
    using Durable = persistence::ProjectPersistenceState;
    switch(state) {
    case ProjectLifecycleState::Closed: return persistence_.saveProjectLifecycle(id, Durable::Closed);
    case ProjectLifecycleState::Opening: return persistence_.saveProjectLifecycle(id, Durable::Opening);
    case ProjectLifecycleState::Open: return persistence_.saveProjectLifecycle(id, Durable::Open);
    case ProjectLifecycleState::Closing: return persistence_.saveProjectLifecycle(id, Durable::Closing);
    case ProjectLifecycleState::Failed: return persistence_.saveProjectLifecycle(id, Durable::Failed);
    }
    return foundation::Result<void>::failure(projectError("Project.InvalidState", id));
}

foundation::Result<ProjectLifecycleSnapshot> ProjectRuntime::fail(const kernel::ProjectId& id, foundation::Error error)
{
    // Keep the original failure even if recording Failed also fails.
    // 中文翻译：记录 Failed 也失败时仍保留原始错误，并附带持久化失败原因。
    auto saved = persist(id, ProjectLifecycleState::Failed);
    if(!saved) {
        error = foundation::makeError("Project.FailureRecordingFailed", foundation::ErrorCategory::Infrastructure,
            "The project failed and its failure state could not be persisted",
            foundation::Value{foundation::Value::Object{
                {"persistenceError", foundation::Value{std::string(saved.error().code.value())}}}},
            foundation::Severity::Error, std::make_shared<const foundation::Error>(std::move(error)));
    }
    std::lock_guard lock(mutex_);
    auto& entry = entries_.at(id);
    entry.state = ProjectLifecycleState::Failed;
    entry.error = error;
    return foundation::Result<ProjectLifecycleSnapshot>::failure(std::move(error));
}

foundation::Result<ProjectLifecycleSnapshot> ProjectRuntime::finishOpen(const kernel::ProjectId& id)
{
    auto saved = persist(id, ProjectLifecycleState::Opening);
    if(!saved) { return fail(id, std::move(saved).error()); }
    saved = persist(id, ProjectLifecycleState::Open);
    if(!saved) { return fail(id, std::move(saved).error()); }
    std::lock_guard lock(mutex_);
    auto& entry = entries_.at(id);
    entry.state = ProjectLifecycleState::Open;
    entry.error.reset();
    return foundation::Result<ProjectLifecycleSnapshot>::success({id, entry.state, entry.activities, entry.error});
}

foundation::Result<ProjectLifecycleSnapshot> ProjectRuntime::create(kernel::ProjectId id)
{
    auto admitted = kernel::ExecutionAdmission::acquire(admission_, "Project.RuntimeNotAccepting");
    if(!admitted) { return foundation::Result<ProjectLifecycleSnapshot>::failure(std::move(admitted).error()); }
    {
        std::lock_guard lock(mutex_);
        if(!accepting()) { return foundation::Result<ProjectLifecycleSnapshot>::failure(projectError("Project.RuntimeNotAccepting", id)); }
        if(entries_.contains(id)) { return foundation::Result<ProjectLifecycleSnapshot>::failure(projectError("Project.AlreadyExists", id)); }
        entries_.emplace(id, Entry{ProjectLifecycleState::Opening, 0U, std::nullopt});
    }
    return finishOpen(id);
}

foundation::Result<ProjectLifecycleSnapshot> ProjectRuntime::open(const kernel::ProjectId& id)
{
    auto admitted = kernel::ExecutionAdmission::acquire(admission_, "Project.RuntimeNotAccepting");
    if(!admitted) { return foundation::Result<ProjectLifecycleSnapshot>::failure(std::move(admitted).error()); }
    {
        std::lock_guard lock(mutex_);
        if(!accepting()) { return foundation::Result<ProjectLifecycleSnapshot>::failure(projectError("Project.RuntimeNotAccepting", id)); }
        const auto found = entries_.find(id);
        if(found == entries_.end()) { return foundation::Result<ProjectLifecycleSnapshot>::failure(projectError("Project.NotFound", id)); }
        if(found->second.state != ProjectLifecycleState::Closed) {
            return foundation::Result<ProjectLifecycleSnapshot>::failure(projectError("Project.NotClosed", id));
        }
        found->second.state = ProjectLifecycleState::Opening;
    }
    return finishOpen(id);
}

foundation::Result<ProjectLifecycleSnapshot> ProjectRuntime::close(const kernel::ProjectId& id)
{
    auto admitted = kernel::ExecutionAdmission::acquire(admission_, "Project.RuntimeNotAccepting");
    if(!admitted) { return foundation::Result<ProjectLifecycleSnapshot>::failure(std::move(admitted).error()); }
    {
        std::lock_guard lock(mutex_);
        if(!accepting()) { return foundation::Result<ProjectLifecycleSnapshot>::failure(projectError("Project.RuntimeNotAccepting", id)); }
        const auto found = entries_.find(id);
        if(found == entries_.end()) { return foundation::Result<ProjectLifecycleSnapshot>::failure(projectError("Project.NotFound", id)); }
        if(found->second.state != ProjectLifecycleState::Open) { return foundation::Result<ProjectLifecycleSnapshot>::failure(projectError("Project.NotOpen", id)); }
        if(found->second.activities != 0U) { return foundation::Result<ProjectLifecycleSnapshot>::failure(projectError("Project.CloseBlocked", id)); }
        found->second.state = ProjectLifecycleState::Closing;
    }
    if(documents_ == nullptr) { return fail(id, projectError("Project.DocumentRuntimeMissing", id)); }
    auto preflight = [&]() -> foundation::Result<std::vector<kernel::DocumentId>> {
        try {
            // Admission is sealed before probing long-lived ownership, including document-free tasks.
            // 中文翻译：先封闭项目准入再检查长期所有权，不能遗漏不携带文档的任务。
            if(taskBlocker_ && taskBlocker_(id) != 0U) {
                return foundation::Result<std::vector<kernel::DocumentId>>::failure(projectError("Project.CloseBlocked", id));
            }
            return documents_->preflightProjectClose(id);
        }
        catch(...) { return foundation::Result<std::vector<kernel::DocumentId>>::failure(projectError("Project.CloseProbeFailed", id)); }
    }();
    if(!preflight) {
        if(preflight.error().code.value() == "Project.CloseProbeFailed") {
            return fail(id, std::move(preflight).error());
        }
        std::lock_guard lock(mutex_);
        entries_.at(id).state = ProjectLifecycleState::Open;
        return foundation::Result<ProjectLifecycleSnapshot>::failure(std::move(preflight).error());
    }
    auto saved = persist(id, ProjectLifecycleState::Closing);
    if(!saved) { return fail(id, std::move(saved).error()); }
    for(const auto& document : preflight.value()) {
        auto closed = documents_->closeForProject(id, document);
        if(!closed) { return fail(id, std::move(closed).error()); }
    }
    saved = persist(id, ProjectLifecycleState::Closed);
    if(!saved) { return fail(id, std::move(saved).error()); }
    std::lock_guard lock(mutex_);
    auto& entry = entries_.at(id);
    entry.state = ProjectLifecycleState::Closed;
    entry.error.reset();
    return foundation::Result<ProjectLifecycleSnapshot>::success({id, entry.state, entry.activities, entry.error});
}

foundation::Result<ProjectLifecycleSnapshot> ProjectRuntime::lifecycle(const kernel::ProjectId& id) const
{
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(id);
    if(found == entries_.end()) { return foundation::Result<ProjectLifecycleSnapshot>::failure(projectError("Project.NotFound", id)); }
    const auto& entry = found->second;
    return foundation::Result<ProjectLifecycleSnapshot>::success({id, entry.state, entry.activities, entry.error});
}

std::vector<ProjectLifecycleSnapshot> ProjectRuntime::list() const
{
    std::lock_guard lock(mutex_);
    std::vector<ProjectLifecycleSnapshot> result;
    result.reserve(entries_.size());
    for(const auto& [id, entry] : entries_) { result.push_back({id, entry.state, entry.activities, entry.error}); }
    return result;
}

foundation::Result<void> ProjectRuntime::configureProject(const kernel::ProjectId& id)
{
    std::lock_guard lock(mutex_);
    entries_.try_emplace(id, Entry{ProjectLifecycleState::Open, 0U, std::nullopt});
    return foundation::Result<void>::success();
}

foundation::Result<void> ProjectRuntime::adoptCatalog(const std::vector<persistence::ProjectCatalogRecord>& records)
{
    std::lock_guard lock(mutex_);
    auto next = entries_;
    for(const auto& record : records) {
        ProjectLifecycleState state{ProjectLifecycleState::Failed};
        switch(record.state) {
        case persistence::ProjectPersistenceState::Closed: state = ProjectLifecycleState::Closed; break;
        case persistence::ProjectPersistenceState::Open: state = ProjectLifecycleState::Open; break;
        case persistence::ProjectPersistenceState::Opening:
        case persistence::ProjectPersistenceState::Closing:
        case persistence::ProjectPersistenceState::Failed: break;
        }
        if(next.contains(record.projectId) && state != ProjectLifecycleState::Open) {
            return foundation::Result<void>::failure(projectError("Project.RecoveryStateConflict", record.projectId));
        }
        std::optional<foundation::Error> error;
        if(state == ProjectLifecycleState::Failed) {
            error = projectError(record.interruptedTransition ? "Project.RecoveryInterruptedTransition" : "Project.RecoveryFailedState", record.projectId);
        }
        next.insert_or_assign(record.projectId, Entry{state, 0U, std::move(error)});
    }
    entries_.swap(next);
    return foundation::Result<void>::success();
}

foundation::Result<ProjectActivityLease> ProjectRuntime::acquireActivity(const kernel::ProjectId& id) const
{
    // Allocate before locking; a failed allocation must not invoke a locked release callback.
    // 中文翻译：加锁前分配租约，分配失败不能回调仍被持有的互斥锁。
    ProjectActivityLease lease;
    try { lease.token_ = std::make_shared<ProjectActivityLease::Token>(id); }
    catch(...) { return foundation::Result<ProjectActivityLease>::failure(projectError("Project.ActivityAdmissionFailed", id)); }
    std::lock_guard lock(mutex_);
    if(!accepting()) { return foundation::Result<ProjectActivityLease>::failure(projectError("Project.RuntimeNotAccepting", id)); }
    const auto found = entries_.find(id);
    if(found == entries_.end() || found->second.state != ProjectLifecycleState::Open) {
        return foundation::Result<ProjectActivityLease>::failure(projectError("Project.NotOpen", id));
    }
    ++found->second.activities;
    lease.token_->owner = this;
    return foundation::Result<ProjectActivityLease>::success(std::move(lease));
}

void ProjectRuntime::releaseActivity(const kernel::ProjectId& id) const noexcept
{
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(id);
    if(found != entries_.end() && found->second.activities != 0U) { --found->second.activities; }
}
bool ProjectRuntime::accepting() const noexcept { return accepting_.load(std::memory_order_acquire); }
void ProjectRuntime::start() noexcept { accepting_.store(true, std::memory_order_release); }
void ProjectRuntime::stop() noexcept { accepting_.store(false, std::memory_order_release); }
} // namespace lasercnc::runtime
