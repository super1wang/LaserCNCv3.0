#include <lasercnc/runtime/document_runtime.hpp>
#include "../../kernel/execution_admission.hpp"

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/persistence/persistence_service.hpp>
#include <lasercnc/state/document_store.hpp>
#include <lasercnc/state/object_type_registry.hpp>
#include <lasercnc/runtime/asset_validation.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <span>
#include <string>
#include <utility>

namespace lasercnc::runtime {
namespace {

foundation::Error documentRuntimeError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const kernel::DocumentId& documentId,
    foundation::Value::Object details = {},
    std::shared_ptr<const foundation::Error> cause = nullptr)
{
    details.emplace(
        "documentId", foundation::Value {std::string(documentId.value())});
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {std::move(details)},
        foundation::Severity::Error,
        std::move(cause));
}

std::size_t activityIndex(DocumentActivityKind kind) noexcept
{
    return static_cast<std::size_t>(kind);
}

std::size_t activityCount(const std::array<std::size_t, 6U>& activities) noexcept
{
    std::size_t result = 0U;
    for(const auto count : activities) {
        result += count;
    }
    return result;
}

foundation::Result<kernel::SnapshotId> closeSnapshotId(
    const kernel::DocumentId& documentId)
{
    static std::atomic_ullong sequence {0U};
    const auto epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return kernel::SnapshotId::create(
        "snapshot.close." + std::string(documentId.value()) + "."
        + std::to_string(epoch) + "."
        + std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed)));
}

} // namespace

struct DocumentRuntime::ActivityToken final {
    ActivityToken(kernel::DocumentId id, DocumentActivityKind activity, ProjectActivityLease project)
        : documentId(std::move(id)), kind(activity), projectLease(std::move(project)) {}
    ~ActivityToken() { if(owner != nullptr) { owner->releaseActivity(documentId, kind); } }
    kernel::DocumentId documentId;
    DocumentActivityKind kind;
    ProjectActivityLease projectLease;
    const DocumentRuntime* owner{nullptr};
};

const char* documentLifecycleStateName(DocumentLifecycleState state) noexcept
{
    switch(state) {
    case DocumentLifecycleState::Detached: return "detached";
    case DocumentLifecycleState::Opening: return "opening";
    case DocumentLifecycleState::Open: return "open";
    case DocumentLifecycleState::Closing: return "closing";
    case DocumentLifecycleState::Failed: return "failed";
    }
    return "unknown";
}

DocumentRuntime::DocumentRuntime(
    state::DocumentStore& documents,
    persistence::PersistenceService& persistence,
    const state::ObjectTypeRegistry* objectTypes,
    const platform::IAssetStore* assetStore) noexcept
    : documents_(documents), persistence_(persistence), objectTypes_(objectTypes), assetStore_(assetStore)
{
}

foundation::Result<DocumentLifecycleSnapshot> DocumentRuntime::create(
    kernel::ProjectId projectId,
    kernel::DocumentId documentId)
{
    auto admitted = kernel::ExecutionAdmission::acquire(admission_, "Document.RuntimeNotAccepting");
    if(!admitted) { return foundation::Result<DocumentLifecycleSnapshot>::failure(std::move(admitted).error()); }
    if(!accepting()) {
        return foundation::Result<DocumentLifecycleSnapshot>::failure(
            documentRuntimeError(
                "Document.RuntimeNotAccepting",
                foundation::ErrorCategory::Conflict,
                "The document runtime is not accepting lifecycle operations",
                documentId));
    }
    auto projectActivity = projects_ != nullptr ? projects_->acquireActivity(projectId)
        : foundation::Result<ProjectActivityLease>::success({});
    if(!projectActivity) {
        return foundation::Result<DocumentLifecycleSnapshot>::failure(std::move(projectActivity).error());
    }
    {
        std::lock_guard lock(mutex_);
        const auto existing = entries_.find(documentId);
        if(existing != entries_.end()
           && existing->second.state != DocumentLifecycleState::Detached) {
            return foundation::Result<DocumentLifecycleSnapshot>::failure(
                documentRuntimeError(
                    "Document.LifecycleConflict",
                    foundation::ErrorCategory::Conflict,
                    "The document is already attached or changing lifecycle state",
                    documentId));
        }
        if(existing != entries_.end() && existing->second.projectId != projectId) {
            return foundation::Result<DocumentLifecycleSnapshot>::failure(
                documentRuntimeError(
                    "Document.OwnershipConflict",
                    foundation::ErrorCategory::Conflict,
                    "The document identity is already bound to another project",
                    documentId));
        }
        entries_.insert_or_assign(
            documentId,
            Entry {projectId, DocumentLifecycleState::Opening, {}, std::nullopt});
    }
    if(persistence_.configured()) {
        auto persisted = persistence_.saveDocumentLifecycle(
            projectId,
            documentId,
            persistence::DocumentPersistenceState::Opening);
        if(!persisted) {
            std::lock_guard lock(mutex_);
            auto& entry = entries_.at(documentId);
            entry.state = DocumentLifecycleState::Failed;
            entry.error = persisted.error();
            return foundation::Result<DocumentLifecycleSnapshot>::failure(
                std::move(persisted).error());
        }
    }
    auto added = documents_.addDocument(projectId, documentId);
    if(!added) {
        std::lock_guard lock(mutex_);
        auto& entry = entries_.at(documentId);
        entry.state = DocumentLifecycleState::Failed;
        entry.error = added.error();
        return foundation::Result<DocumentLifecycleSnapshot>::failure(
            std::move(added).error());
    }
    if(persistence_.configured()) {
        auto persisted = persistence_.saveDocumentLifecycle(
            projectId,
            documentId,
            persistence::DocumentPersistenceState::Open);
        if(!persisted) {
            std::lock_guard lock(mutex_);
            auto& entry = entries_.at(documentId);
            entry.state = DocumentLifecycleState::Failed;
            entry.error = persisted.error();
            return foundation::Result<DocumentLifecycleSnapshot>::failure(
                std::move(persisted).error());
        }
    }
    std::lock_guard lock(mutex_);
    auto& entry = entries_.at(documentId);
    entry.state = DocumentLifecycleState::Open;
    entry.error.reset();
    return foundation::Result<DocumentLifecycleSnapshot>::success(
        snapshotOf(documentId, entry));
}

foundation::Result<DocumentLifecycleSnapshot> DocumentRuntime::attach(
    state::DocumentImage image)
{
    auto kernelAdmission = kernel::ExecutionAdmission::acquire(admission_, "Document.RuntimeNotAccepting");
    if(!kernelAdmission) { return foundation::Result<DocumentLifecycleSnapshot>::failure(std::move(kernelAdmission).error()); }
    if(!accepting()) {
        return foundation::Result<DocumentLifecycleSnapshot>::failure(
            documentRuntimeError(
                "Document.RuntimeNotAccepting",
                foundation::ErrorCategory::Conflict,
                "The document runtime is not accepting lifecycle operations",
                image.documentId));
    }
    const auto documentId = image.documentId;
    const auto projectId = image.projectId;
    auto projectActivity = projects_ != nullptr ? projects_->acquireActivity(projectId)
        : foundation::Result<ProjectActivityLease>::success({});
    if(!projectActivity) {
        return foundation::Result<DocumentLifecycleSnapshot>::failure(std::move(projectActivity).error());
    }
    auto assetsAdmitted = validateObjectAssets(image.objects, assetStore_);
    if(!assetsAdmitted) {
        return foundation::Result<DocumentLifecycleSnapshot>::failure(std::move(assetsAdmitted).error());
    }
    if(objectTypes_ != nullptr) {
        auto admitted = objectTypes_->validateObjects(image.objects, persistence_.configured());
        if(!admitted) {
            return foundation::Result<DocumentLifecycleSnapshot>::failure(std::move(admitted).error());
        }
    }
    {
        std::lock_guard lock(mutex_);
        const auto existing = entries_.find(documentId);
        if(existing != entries_.end()
           && existing->second.state != DocumentLifecycleState::Detached) {
            return foundation::Result<DocumentLifecycleSnapshot>::failure(
                documentRuntimeError(
                    "Document.LifecycleConflict",
                    foundation::ErrorCategory::Conflict,
                    "The document is already attached or changing lifecycle state",
                    documentId));
        }
        if(existing != entries_.end() && existing->second.projectId != projectId) {
            return foundation::Result<DocumentLifecycleSnapshot>::failure(
                documentRuntimeError(
                    "Document.OwnershipConflict",
                    foundation::ErrorCategory::Conflict,
                    "The document identity is already bound to another project",
                    documentId));
        }
        entries_.insert_or_assign(
            documentId,
            Entry {projectId, DocumentLifecycleState::Opening, {}, std::nullopt});
    }
    if(persistence_.configured()) {
        auto persisted = persistence_.saveDocumentLifecycle(
            projectId,
            documentId,
            persistence::DocumentPersistenceState::Opening);
        if(!persisted) {
            std::lock_guard lock(mutex_);
            auto& entry = entries_.at(documentId);
            entry.state = DocumentLifecycleState::Failed;
            entry.error = persisted.error();
            return foundation::Result<DocumentLifecycleSnapshot>::failure(
                std::move(persisted).error());
        }
    }
    const std::array images {std::move(image)};
    auto restored = documents_.restoreDocuments(images);
    if(!restored) {
        std::lock_guard lock(mutex_);
        auto& entry = entries_.at(documentId);
        entry.state = DocumentLifecycleState::Failed;
        entry.error = restored.error();
        return foundation::Result<DocumentLifecycleSnapshot>::failure(
            std::move(restored).error());
    }
    if(persistence_.configured()) {
        auto persisted = persistence_.saveDocumentLifecycle(
            projectId,
            documentId,
            persistence::DocumentPersistenceState::Open);
        if(!persisted) {
            std::lock_guard lock(mutex_);
            auto& entry = entries_.at(documentId);
            entry.state = DocumentLifecycleState::Failed;
            entry.error = persisted.error();
            return foundation::Result<DocumentLifecycleSnapshot>::failure(
                std::move(persisted).error());
        }
    }
    std::lock_guard lock(mutex_);
    auto& entry = entries_.at(documentId);
    entry.state = DocumentLifecycleState::Open;
    entry.error.reset();
    return foundation::Result<DocumentLifecycleSnapshot>::success(
        snapshotOf(documentId, entry));
}

foundation::Result<DocumentLifecycleSnapshot> DocumentRuntime::open(
    const kernel::DocumentId& documentId)
{
    auto admitted = kernel::ExecutionAdmission::acquire(admission_, "Document.RuntimeNotAccepting");
    if(!admitted) { return foundation::Result<DocumentLifecycleSnapshot>::failure(std::move(admitted).error()); }
    if(!accepting()) {
        return foundation::Result<DocumentLifecycleSnapshot>::failure(
            documentRuntimeError(
                "Document.RuntimeNotAccepting",
                foundation::ErrorCategory::Conflict,
                "The document runtime is not accepting lifecycle operations",
                documentId));
    }
    if(!persistence_.configured()) {
        return foundation::Result<DocumentLifecycleSnapshot>::failure(
            documentRuntimeError(
                "Document.OpenRequiresPersistence",
                foundation::ErrorCategory::Conflict,
                "Opening a detached document requires configured persistence",
                documentId));
    }
    auto catalog = persistence_.documentCatalog();
    if(!catalog) {
        return foundation::Result<DocumentLifecycleSnapshot>::failure(
            std::move(catalog).error());
    }
    const auto record = std::find_if(
        catalog.value().begin(),
        catalog.value().end(),
        [&](const auto& candidate) { return candidate.documentId == documentId; });
    if(record == catalog.value().end()) {
        return foundation::Result<DocumentLifecycleSnapshot>::failure(
            documentRuntimeError(
                "Document.LifecycleNotFound",
                foundation::ErrorCategory::NotFound,
                "The durable document lifecycle entry does not exist",
                documentId));
    }
    if(record->state != persistence::DocumentPersistenceState::Detached) {
        return foundation::Result<DocumentLifecycleSnapshot>::failure(
            documentRuntimeError(
                "Document.NotDetached",
                foundation::ErrorCategory::Conflict,
                "Only a durably detached document can be opened",
                documentId));
    }
    auto projectActivity = projects_ != nullptr ? projects_->acquireActivity(record->projectId)
        : foundation::Result<ProjectActivityLease>::success({});
    if(!projectActivity) {
        return foundation::Result<DocumentLifecycleSnapshot>::failure(std::move(projectActivity).error());
    }
    auto recovered = persistence_.recover();
    if(!recovered) {
        return foundation::Result<DocumentLifecycleSnapshot>::failure(
            std::move(recovered).error());
    }
    auto image = std::find_if(
        recovered.value().documents.begin(),
        recovered.value().documents.end(),
        [&](const auto& candidate) { return candidate.documentId == documentId; });
    if(image != recovered.value().documents.end()) {
        return attach(*image);
    }
    state::Revision projectRevision;
    for(const auto& recoveredImage : recovered.value().documents) {
        if(recoveredImage.projectId == record->projectId) {
            projectRevision = recoveredImage.revisions.at(
                state::RevisionScope::Project);
            break;
        }
    }
    return attach(state::DocumentImage {
        record->projectId,
        record->documentId,
        state::RevisionSet {
            projectRevision,
            state::Revision {},
            state::Revision {},
            state::Revision {},
            state::Revision {},
            state::Revision {}},
        {}});
}

foundation::Result<state::Document> DocumentRuntime::snapshot(
    const kernel::DocumentId& documentId) const
{
    auto activity = acquireActivity(documentId, DocumentActivityKind::Query);
    if(!activity) {
        return foundation::Result<state::Document>::failure(
            std::move(activity).error());
    }
    return documents_.snapshot(documentId);
}

foundation::Result<DocumentLifecycleSnapshot> DocumentRuntime::close(
    const kernel::DocumentId& documentId)
{
    auto admitted = kernel::ExecutionAdmission::acquire(admission_, "Document.RuntimeNotAccepting");
    if(!admitted) { return foundation::Result<DocumentLifecycleSnapshot>::failure(std::move(admitted).error()); }
    auto projectActivity = acquireProject(documentId);
    if(!projectActivity) {
        return foundation::Result<DocumentLifecycleSnapshot>::failure(std::move(projectActivity).error());
    }
    return detachImpl(documentId, true, &projectActivity.value());
}

foundation::Result<DocumentLifecycleSnapshot> DocumentRuntime::detach(
    const kernel::DocumentId& documentId)
{
    auto admitted = kernel::ExecutionAdmission::acquire(admission_, "Document.RuntimeNotAccepting");
    if(!admitted) { return foundation::Result<DocumentLifecycleSnapshot>::failure(std::move(admitted).error()); }
    if(persistence_.configured()) {
        return foundation::Result<DocumentLifecycleSnapshot>::failure(
            documentRuntimeError(
                "Document.DetachWouldBypassPersistence",
                foundation::ErrorCategory::Conflict,
                "Configured persistence requires close instead of direct detach",
                documentId));
    }
    auto projectActivity = acquireProject(documentId);
    if(!projectActivity) {
        return foundation::Result<DocumentLifecycleSnapshot>::failure(std::move(projectActivity).error());
    }
    return detachImpl(documentId, false, &projectActivity.value());
}

foundation::Result<DocumentLifecycleSnapshot> DocumentRuntime::detachImpl(
    const kernel::DocumentId& documentId,
    bool persist,
    const ProjectActivityLease* projectLease)
{
    if(!accepting()) {
        return foundation::Result<DocumentLifecycleSnapshot>::failure(
            documentRuntimeError(
                "Document.RuntimeNotAccepting",
                foundation::ErrorCategory::Conflict,
                "The document runtime is not accepting lifecycle operations",
                documentId));
    }
    CloseBlockers blockers;
    {
        std::lock_guard lock(mutex_);
        const auto found = entries_.find(documentId);
        if(found == entries_.end()) {
            return foundation::Result<DocumentLifecycleSnapshot>::failure(
                documentRuntimeError(
                    "Document.LifecycleNotFound",
                    foundation::ErrorCategory::NotFound,
                    "The document lifecycle entry does not exist",
                    documentId));
        }
        if(found->second.state != DocumentLifecycleState::Open) {
            return foundation::Result<DocumentLifecycleSnapshot>::failure(
                documentRuntimeError(
                    "Document.NotOpen",
                    foundation::ErrorCategory::Conflict,
                    "Only an open document can begin closing",
                    documentId));
        }
        if(projectLease != nullptr && !projectLease->matches(found->second.projectId)) {
            return foundation::Result<DocumentLifecycleSnapshot>::failure(documentRuntimeError(
                "Document.OwnershipConflict", foundation::ErrorCategory::Conflict,
                "The document ownership changed during lifecycle admission", documentId));
        }
        const auto activities = activityCount(found->second.activities);
        if(activities != 0U) {
            return foundation::Result<DocumentLifecycleSnapshot>::failure(
                documentRuntimeError(
                    "Document.ActiveOperations",
                    foundation::ErrorCategory::Conflict,
                    "The document cannot close while lifecycle operations are active",
                    documentId,
                    {{"activeOperationCount",
                      foundation::Value {std::to_string(activities)}}}));
        }
        found->second.state = DocumentLifecycleState::Closing;
        found->second.error.reset();
        blockers = blockers_;
    }

    if(persist && persistence_.configured()) {
        kernel::ProjectId projectId = [&]() {
            std::lock_guard lock(mutex_);
            return entries_.at(documentId).projectId;
        }();
        auto persisted = persistence_.saveDocumentLifecycle(
            projectId,
            documentId,
            persistence::DocumentPersistenceState::Closing);
        if(!persisted) {
            std::lock_guard lock(mutex_);
            auto& entry = entries_.at(documentId);
            entry.state = DocumentLifecycleState::Failed;
            entry.error = persisted.error();
            return foundation::Result<DocumentLifecycleSnapshot>::failure(
                std::move(persisted).error());
        }
    }

    std::array<std::size_t, 4U> blockerCounts{};
    try {
        blockerCounts[0] = blockers.transactions ? blockers.transactions(documentId) : 0U;
        blockerCounts[1] = blockers.tasks ? blockers.tasks(documentId) : 0U;
        blockerCounts[2] = blockers.workflows ? blockers.workflows(documentId) : 0U;
        blockerCounts[3] = blockers.scripts ? blockers.scripts(documentId) : 0U;
    } catch(const std::exception& exception) {
        auto error = documentRuntimeError(
            "Document.CloseInspectionFailed",
            foundation::ErrorCategory::Internal,
            "Document close blocker inspection failed unexpectedly",
            documentId,
            {{"reason", foundation::Value {exception.what()}}});
        std::lock_guard lock(mutex_);
        auto& entry = entries_.at(documentId);
        entry.state = DocumentLifecycleState::Failed;
        entry.error = error;
        return foundation::Result<DocumentLifecycleSnapshot>::failure(std::move(error));
    } catch(...) {
        auto error = documentRuntimeError(
            "Document.CloseInspectionFailed",
            foundation::ErrorCategory::Internal,
            "Document close blocker inspection failed unexpectedly",
            documentId);
        std::lock_guard lock(mutex_);
        auto& entry = entries_.at(documentId);
        entry.state = DocumentLifecycleState::Failed;
        entry.error = error;
        return foundation::Result<DocumentLifecycleSnapshot>::failure(std::move(error));
    }
    const auto blockerCount = blockerCounts[0] + blockerCounts[1]
        + blockerCounts[2] + blockerCounts[3];
    if(blockerCount != 0U) {
        if(persist && persistence_.configured()) {
            kernel::ProjectId projectId = [&]() {
                std::lock_guard lock(mutex_);
                return entries_.at(documentId).projectId;
            }();
            auto restored = persistence_.saveDocumentLifecycle(
                projectId,
                documentId,
                persistence::DocumentPersistenceState::Open);
            if(!restored) {
                std::lock_guard lock(mutex_);
                auto& entry = entries_.at(documentId);
                entry.state = DocumentLifecycleState::Failed;
                entry.error = restored.error();
                return foundation::Result<DocumentLifecycleSnapshot>::failure(
                    std::move(restored).error());
            }
        }
        std::lock_guard lock(mutex_);
        auto& entry = entries_.at(documentId);
        entry.state = DocumentLifecycleState::Open;
        return foundation::Result<DocumentLifecycleSnapshot>::failure(
            documentRuntimeError(
                "Document.CloseBlocked",
                foundation::ErrorCategory::Conflict,
                "The document cannot close while transactions, tasks or orchestrations remain active",
                documentId,
                {{"activeScriptCount", foundation::Value {std::to_string(blockerCounts[3])}},
                 {"activeTaskCount", foundation::Value {std::to_string(blockerCounts[1])}},
                 {"activeTransactionCount", foundation::Value {std::to_string(blockerCounts[0])}},
                 {"activeWorkflowCount", foundation::Value {std::to_string(blockerCounts[2])}}}));
    }

    if(persist && persistence_.configured()) {
        auto document = documents_.snapshot(documentId);
        if(!document) {
            std::lock_guard lock(mutex_);
            auto& entry = entries_.at(documentId);
            entry.state = DocumentLifecycleState::Failed;
            entry.error = document.error();
            return foundation::Result<DocumentLifecycleSnapshot>::failure(
                std::move(document).error());
        }
        auto snapshotId = closeSnapshotId(documentId);
        auto assetsAdmitted = validateObjectAssets(document.value().objects().all(), assetStore_);
        auto captured = !assetsAdmitted
            ? foundation::Result<persistence::SnapshotRecord>::failure(std::move(assetsAdmitted).error())
            : snapshotId
            ? persistence_.captureSnapshot(
                  std::move(snapshotId).value(), document.value())
            : foundation::Result<persistence::SnapshotRecord>::failure(
                  std::move(snapshotId).error());
        if(!captured) {
            const auto projectId = document.value().projectId();
            static_cast<void>(persistence_.saveDocumentLifecycle(
                projectId,
                documentId,
                persistence::DocumentPersistenceState::Failed));
            std::lock_guard lock(mutex_);
            auto& entry = entries_.at(documentId);
            entry.state = DocumentLifecycleState::Failed;
            entry.error = captured.error();
            return foundation::Result<DocumentLifecycleSnapshot>::failure(
                std::move(captured).error());
        }
        auto persistedDetached = persistence_.saveDocumentLifecycle(
            document.value().projectId(),
            documentId,
            persistence::DocumentPersistenceState::Detached);
        if(!persistedDetached) {
            std::lock_guard lock(mutex_);
            auto& entry = entries_.at(documentId);
            entry.state = DocumentLifecycleState::Failed;
            entry.error = persistedDetached.error();
            return foundation::Result<DocumentLifecycleSnapshot>::failure(
                std::move(persistedDetached).error());
        }
    }

    auto detached = documents_.detachDocument(documentId);
    std::lock_guard lock(mutex_);
    auto& entry = entries_.at(documentId);
    if(!detached) {
        if(persist && persistence_.configured()) {
            static_cast<void>(persistence_.saveDocumentLifecycle(
                entry.projectId,
                documentId,
                persistence::DocumentPersistenceState::Failed));
        }
        entry.state = DocumentLifecycleState::Failed;
        entry.error = detached.error();
        return foundation::Result<DocumentLifecycleSnapshot>::failure(
            std::move(detached).error());
    }
    entry.state = DocumentLifecycleState::Detached;
    entry.error.reset();
    return foundation::Result<DocumentLifecycleSnapshot>::success(
        snapshotOf(documentId, entry));
}

foundation::Result<void> DocumentRuntime::remove(
    const kernel::DocumentId& documentId)
{
    auto admitted = kernel::ExecutionAdmission::acquire(admission_, "Document.RuntimeNotAccepting");
    if(!admitted) { return foundation::Result<void>::failure(std::move(admitted).error()); }
    auto projectActivity = acquireProject(documentId);
    if(!projectActivity) { return foundation::Result<void>::failure(std::move(projectActivity).error()); }
    if(!accepting()) {
        return foundation::Result<void>::failure(documentRuntimeError(
            "Document.RuntimeNotAccepting",
            foundation::ErrorCategory::Conflict,
            "The document runtime is not accepting lifecycle operations",
            documentId));
    }
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(documentId);
    if(found == entries_.end()) {
        return foundation::Result<void>::failure(documentRuntimeError(
            "Document.LifecycleNotFound",
            foundation::ErrorCategory::NotFound,
            "The document lifecycle entry does not exist",
            documentId));
    }
    if(!projectActivity.value().matches(found->second.projectId)) {
        return foundation::Result<void>::failure(documentRuntimeError(
            "Document.OwnershipConflict", foundation::ErrorCategory::Conflict,
            "The document ownership changed during removal admission", documentId));
    }
    if(found->second.state != DocumentLifecycleState::Detached
       || documents_.contains(documentId)) {
        return foundation::Result<void>::failure(documentRuntimeError(
            "Document.RemoveRequiresDetached",
            foundation::ErrorCategory::Conflict,
            "Only a detached document can be removed from the runtime catalog",
            documentId));
    }
    if(persistence_.configured()) {
        auto removed = persistence_.removeDocumentLifecycle(
            found->second.projectId, documentId);
        if(!removed) {
            return removed;
        }
    }
    entries_.erase(found);
    return foundation::Result<void>::success();
}

foundation::Result<DocumentLifecycleSnapshot> DocumentRuntime::lifecycle(
    const kernel::DocumentId& documentId) const
{
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(documentId);
    if(found == entries_.end()) {
        return foundation::Result<DocumentLifecycleSnapshot>::failure(
            documentRuntimeError(
                "Document.LifecycleNotFound",
                foundation::ErrorCategory::NotFound,
                "The document lifecycle entry does not exist",
                documentId));
    }
    return foundation::Result<DocumentLifecycleSnapshot>::success(
        snapshotOf(documentId, found->second));
}

std::vector<DocumentLifecycleSnapshot> DocumentRuntime::list() const
{
    std::lock_guard lock(mutex_);
    std::vector<DocumentLifecycleSnapshot> result;
    result.reserve(entries_.size());
    for(const auto& [documentId, entry] : entries_) {
        result.push_back(snapshotOf(documentId, entry));
    }
    return result;
}

bool DocumentRuntime::accepting() const noexcept
{
    return accepting_.load(std::memory_order_acquire);
}

foundation::Result<void> DocumentRuntime::configureDocument(
    kernel::ProjectId projectId,
    kernel::DocumentId documentId)
{
    {
        std::lock_guard lock(mutex_);
        const auto existing = entries_.find(documentId);
        if(existing != entries_.end()) {
            const auto code = existing->second.projectId == projectId
                ? "Document.LifecycleAlreadyExists"
                : "Document.OwnershipConflict";
            return foundation::Result<void>::failure(documentRuntimeError(
                code,
                foundation::ErrorCategory::Conflict,
                "The document lifecycle identity is already configured",
                documentId));
        }
        entries_.emplace(
            documentId,
            Entry {projectId, DocumentLifecycleState::Opening, {}, std::nullopt});
    }
    auto added = documents_.addDocument(projectId, documentId);
    std::lock_guard lock(mutex_);
    auto& entry = entries_.at(documentId);
    if(!added) {
        entry.state = DocumentLifecycleState::Failed;
        entry.error = added.error();
        return added;
    }
    entry.state = DocumentLifecycleState::Open;
    entry.error.reset();
    return foundation::Result<void>::success();
}

foundation::Result<void> DocumentRuntime::adoptRecovered(
    const std::vector<state::DocumentImage>& images)
{
    std::lock_guard lock(mutex_);
    auto next = entries_;
    for(const auto& image : images) {
        const auto existing = next.find(image.documentId);
        if(existing != next.end() && existing->second.projectId != image.projectId) {
            return foundation::Result<void>::failure(documentRuntimeError(
                "Document.RecoveryOwnershipConflict",
                foundation::ErrorCategory::Conflict,
                "Recovered lifecycle ownership conflicts with the configured document",
                image.documentId));
        }
        next.insert_or_assign(
            image.documentId,
            Entry {image.projectId, DocumentLifecycleState::Open, {}, std::nullopt});
    }
    entries_.swap(next);
    return foundation::Result<void>::success();
}

foundation::Result<void> DocumentRuntime::adoptCatalog(
    const std::vector<persistence::DocumentCatalogRecord>& records)
{
    std::lock_guard lock(mutex_);
    auto next = entries_;
    for(const auto& record : records) {
        if(record.state == persistence::DocumentPersistenceState::Removed) {
            const auto existing = next.find(record.documentId);
            if(existing != next.end()) {
                return foundation::Result<void>::failure(documentRuntimeError(
                    "Document.RecoveryRemovedConflict",
                    foundation::ErrorCategory::Conflict,
                    "A removed durable document conflicts with a configured document",
                    record.documentId));
            }
            continue;
        }
        DocumentLifecycleState state = DocumentLifecycleState::Failed;
        switch(record.state) {
        case persistence::DocumentPersistenceState::Detached:
            state = DocumentLifecycleState::Detached;
            break;
        case persistence::DocumentPersistenceState::Open:
            state = DocumentLifecycleState::Open;
            break;
        case persistence::DocumentPersistenceState::Opening:
        case persistence::DocumentPersistenceState::Closing:
        case persistence::DocumentPersistenceState::Failed:
            state = DocumentLifecycleState::Failed;
            break;
        case persistence::DocumentPersistenceState::Removed:
            break;
        }
        const auto existing = next.find(record.documentId);
        if(existing != next.end()) {
            if(existing->second.projectId != record.projectId) {
                return foundation::Result<void>::failure(documentRuntimeError(
                    "Document.RecoveryOwnershipConflict",
                    foundation::ErrorCategory::Conflict,
                    "Durable lifecycle ownership conflicts with the configured document",
                    record.documentId));
            }
            if(existing->second.state == DocumentLifecycleState::Open
               && state != DocumentLifecycleState::Open) {
                return foundation::Result<void>::failure(documentRuntimeError(
                    "Document.RecoveryStateConflict",
                    foundation::ErrorCategory::Conflict,
                    "Durable lifecycle state conflicts with the configured open document",
                    record.documentId));
            }
        }
        std::optional<foundation::Error> error;
        if(state == DocumentLifecycleState::Failed) {
            error = documentRuntimeError(
                record.interruptedTransition
                    ? "Document.RecoveryInterruptedTransition"
                    : "Document.RecoveryFailedState",
                foundation::ErrorCategory::Infrastructure,
                "The durable document lifecycle requires explicit recovery",
                record.documentId);
        }
        next.insert_or_assign(
            record.documentId,
            Entry {record.projectId, state, {}, std::move(error)});
    }
    entries_.swap(next);
    return foundation::Result<void>::success();
}

void DocumentRuntime::configureCloseBlockers(CloseBlockers blockers)
{
    std::lock_guard lock(mutex_);
    blockers_ = std::move(blockers);
}

foundation::Result<ProjectActivityLease> DocumentRuntime::acquireProject(
    const kernel::DocumentId& documentId) const
{
    if(!accepting()) {
        return foundation::Result<ProjectActivityLease>::failure(documentRuntimeError(
            "Document.RuntimeNotAccepting", foundation::ErrorCategory::Conflict,
            "The document runtime is not accepting lifecycle operations", documentId));
    }
    if(projects_ == nullptr) { return foundation::Result<ProjectActivityLease>::success({}); }
    auto current = lifecycle(documentId);
    if(!current) { return foundation::Result<ProjectActivityLease>::failure(std::move(current).error()); }
    return projects_->acquireActivity(current.value().projectId);
}

foundation::Result<ProjectActivityLease> DocumentRuntime::acquireProjectActivity(
    const kernel::ProjectId& projectId) const
{
    if(projects_ == nullptr) { return foundation::Result<ProjectActivityLease>::success({}); }
    return projects_->acquireActivity(projectId);
}

foundation::Result<std::vector<kernel::DocumentId>> DocumentRuntime::preflightProjectClose(
    const kernel::ProjectId& projectId) const
{
    CloseBlockers blockers;
    std::vector<kernel::DocumentId> result;
    {
        std::lock_guard lock(mutex_);
        blockers = blockers_;
        for(const auto& [documentId, entry] : entries_) {
            if(entry.projectId != projectId) { continue; }
            if(entry.state != DocumentLifecycleState::Open
               && entry.state != DocumentLifecycleState::Detached) {
                return foundation::Result<std::vector<kernel::DocumentId>>::failure(documentRuntimeError(
                    "Project.ChildStateConflict", foundation::ErrorCategory::Conflict,
                    "Every child must be open or detached before project close", documentId));
            }
            if(activityCount(entry.activities) != 0U) {
                return foundation::Result<std::vector<kernel::DocumentId>>::failure(documentRuntimeError(
                    "Project.CloseBlocked", foundation::ErrorCategory::Conflict,
                    "A child document still has active leases", documentId));
            }
            if(entry.state == DocumentLifecycleState::Open) { result.push_back(documentId); }
        }
    }
    try {
        for(const auto& documentId : result) {
            if((blockers.transactions && blockers.transactions(documentId) != 0U)
               || (blockers.tasks && blockers.tasks(documentId) != 0U)
               || (blockers.workflows && blockers.workflows(documentId) != 0U)
               || (blockers.scripts && blockers.scripts(documentId) != 0U)) {
                return foundation::Result<std::vector<kernel::DocumentId>>::failure(documentRuntimeError(
                    "Project.CloseBlocked", foundation::ErrorCategory::Conflict,
                    "A child document still has active runtime work", documentId));
            }
        }
    } catch(...) {
        return foundation::Result<std::vector<kernel::DocumentId>>::failure(foundation::makeError(
            "Project.CloseProbeFailed", foundation::ErrorCategory::Internal,
            "Project child activity could not be inspected"));
    }
    return foundation::Result<std::vector<kernel::DocumentId>>::success(std::move(result));
}

foundation::Result<DocumentLifecycleSnapshot> DocumentRuntime::closeForProject(
    const kernel::ProjectId& projectId, const kernel::DocumentId& documentId)
{
    auto document = lifecycle(documentId);
    if(!document) { return foundation::Result<DocumentLifecycleSnapshot>::failure(std::move(document).error()); }
    if(projects_ == nullptr || document.value().projectId != projectId) {
        return foundation::Result<DocumentLifecycleSnapshot>::failure(documentRuntimeError(
            "Document.OwnershipConflict", foundation::ErrorCategory::Conflict,
            "The project close coordinator does not own this document", documentId));
    }
    auto project = projects_->lifecycle(projectId);
    if(!project || project.value().state != ProjectLifecycleState::Closing) {
        return foundation::Result<DocumentLifecycleSnapshot>::failure(documentRuntimeError(
            "Project.NotClosing", foundation::ErrorCategory::Conflict,
            "The parent project must have sealed admission before coordinated close", documentId));
    }
    return detachImpl(documentId, true);
}

foundation::Result<DocumentActivityLease> DocumentRuntime::acquireActivity(
    const kernel::DocumentId& documentId,
    DocumentActivityKind kind) const
{
    if(!accepting()) {
        return foundation::Result<DocumentActivityLease>::failure(
            documentRuntimeError(
                "Document.RuntimeNotAccepting",
                foundation::ErrorCategory::Conflict,
                "The document runtime is not accepting document activity",
                documentId));
    }
    const auto index = activityIndex(kind);
    if(index >= 6U) {
        return foundation::Result<DocumentActivityLease>::failure(
            documentRuntimeError(
                "Document.InvalidActivityKind",
                foundation::ErrorCategory::Internal,
                "The document activity kind is invalid",
                documentId));
    }
    auto projectActivity = acquireProject(documentId);
    if(!projectActivity) { return foundation::Result<DocumentActivityLease>::failure(std::move(projectActivity).error()); }
    std::shared_ptr<ActivityToken> token;
    try {
        token = std::make_shared<ActivityToken>(documentId, kind, std::move(projectActivity).value());
    } catch(...) {
        return foundation::Result<DocumentActivityLease>::failure(documentRuntimeError(
            "Document.ActivityAdmissionFailed", foundation::ErrorCategory::Internal,
            "The document activity lease could not be allocated", documentId));
    }
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(documentId);
    if(found == entries_.end() || found->second.state != DocumentLifecycleState::Open) {
        return foundation::Result<DocumentActivityLease>::failure(
            documentRuntimeError(
                "Document.NotOpen",
                foundation::ErrorCategory::Conflict,
                "The document is not open for new runtime activity",
                documentId));
    }
    if(!token->projectLease.matches(found->second.projectId)) {
        return foundation::Result<DocumentActivityLease>::failure(documentRuntimeError(
            "Document.OwnershipConflict", foundation::ErrorCategory::Conflict,
            "The document ownership changed during activity admission", documentId));
    }
    ++found->second.activities[index];
    token->owner = this;
    return foundation::Result<DocumentActivityLease>::success(DocumentActivityLease{std::move(token)});
}

void DocumentRuntime::releaseActivity(
    const kernel::DocumentId& documentId,
    DocumentActivityKind kind) const noexcept
{
    try {
        std::lock_guard lock(mutex_);
        const auto found = entries_.find(documentId);
        const auto index = activityIndex(kind);
        if(found != entries_.end() && index < found->second.activities.size()
           && found->second.activities[index] != 0U) {
            --found->second.activities[index];
        }
    } catch(...) {
    }
}

DocumentLifecycleSnapshot DocumentRuntime::snapshotOf(
    const kernel::DocumentId& documentId,
    const Entry& entry)
{
    return DocumentLifecycleSnapshot {
        entry.projectId, documentId, entry.state, entry.activities, entry.error};
}

void DocumentRuntime::start() noexcept
{
    accepting_.store(true, std::memory_order_release);
}

void DocumentRuntime::stop() noexcept
{
    accepting_.store(false, std::memory_order_release);
}

} // namespace lasercnc::runtime
