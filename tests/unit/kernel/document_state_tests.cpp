#include <lasercnc/state/document_store.hpp>
#include <lasercnc/state/revision.hpp>
#include <lasercnc/kernel/app_kernel.hpp>
#include <lasercnc/runtime/document_runtime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <string>

using namespace lasercnc::foundation;
using namespace lasercnc::kernel;
using namespace lasercnc::state;

namespace {

template <typename Id>
Id makeId(const char* value)
{
    auto created = Id::create(value);
    REQUIRE(created.hasValue());
    return std::move(created).value();
}

} // namespace

TEST_CASE("Revision models all kernel consistency scopes", "[state][revision]")
{
    RevisionSet revisions;
    CHECK(revisions.at(RevisionScope::Project) == Revision {0U});
    CHECK(revisions.at(RevisionScope::Document) == Revision {0U});
    CHECK(revisions.at(RevisionScope::Geometry) == Revision {0U});
    CHECK(revisions.at(RevisionScope::Cam) == Revision {0U});
    CHECK(revisions.at(RevisionScope::MachineContext) == Revision {0U});
    CHECK(revisions.at(RevisionScope::Environment) == Revision {0U});

    CHECK(std::string(revisionScopeName(RevisionScope::Project)) == "project");
    CHECK(std::string(revisionScopeName(RevisionScope::Document)) == "document");
    CHECK(std::string(revisionScopeName(RevisionScope::Geometry)) == "geometry");
    CHECK(std::string(revisionScopeName(RevisionScope::Cam)) == "cam");
    CHECK(std::string(revisionScopeName(RevisionScope::MachineContext)) == "machineContext");
    CHECK(std::string(revisionScopeName(RevisionScope::Environment)) == "environment");

    const std::array matching {
        RevisionPrecondition {RevisionScope::Project, Revision {0U}},
        RevisionPrecondition {RevisionScope::Geometry, Revision {0U}},
    };
    CHECK(RevisionManager::validate(revisions, matching).hasValue());
}

TEST_CASE("Revision validates conflicts duplicates and overflow", "[state][revision]")
{
    RevisionSet revisions;
    const std::array conflict {
        RevisionPrecondition {RevisionScope::Document, Revision {1U}},
    };
    auto mismatch = RevisionManager::validate(revisions, conflict);
    REQUIRE_FALSE(mismatch.hasValue());
    CHECK(std::string(mismatch.error().code.value()) == "Project.RevisionConflict");

    const std::array duplicate {
        RevisionPrecondition {RevisionScope::Cam, Revision {0U}},
        RevisionPrecondition {RevisionScope::Cam, Revision {0U}},
    };
    auto duplicated = RevisionManager::validate(revisions, duplicate);
    REQUIRE_FALSE(duplicated.hasValue());
    CHECK(std::string(duplicated.error().code.value()) == "Revision.DuplicatePrecondition");

    const std::array invalid {
        RevisionPrecondition {static_cast<RevisionScope>(255), Revision {0U}},
    };
    auto invalidScope = RevisionManager::validate(revisions, invalid);
    REQUIRE_FALSE(invalidScope.hasValue());
    CHECK(std::string(invalidScope.error().code.value()) == "Revision.InvalidScope");

    auto next = Revision {41U}.next();
    REQUIRE(next.hasValue());
    CHECK(next.value() == Revision {42U});
    auto overflow = Revision {std::numeric_limits<std::uint64_t>::max()}.next();
    REQUIRE_FALSE(overflow.hasValue());
    CHECK(std::string(overflow.error().code.value()) == "Revision.Overflow");

    const RevisionSet nearOverflow {
        Revision {9U},
        Revision {std::numeric_limits<std::uint64_t>::max()},
        Revision {4U},
        Revision {3U},
        Revision {2U},
        Revision {1U}};
    const std::array scopes {RevisionScope::Project, RevisionScope::Document};
    auto advanced = RevisionManager::advance(nearOverflow, scopes);
    REQUIRE_FALSE(advanced.hasValue());
    CHECK(std::string(advanced.error().code.value()) == "Revision.Overflow");
    CHECK(nearOverflow.at(RevisionScope::Project) == Revision {9U});
    CHECK(nearOverflow.at(RevisionScope::Document)
          == Revision {std::numeric_limits<std::uint64_t>::max()});
}

TEST_CASE("DocumentStore returns immutable snapshots with stable identity", "[state][document]")
{
    DocumentStore store;
    auto project = makeId<ProjectId>("project.alpha");
    auto firstDocument = makeId<DocumentId>("document.first");
    auto secondDocument = makeId<DocumentId>("document.second");

    REQUIRE(store.addDocument(project, firstDocument).hasValue());
    REQUIRE(store.addDocument(project, secondDocument).hasValue());
    CHECK(store.size() == 2U);
    CHECK(store.contains(firstDocument));

    auto first = store.snapshot(firstDocument);
    REQUIRE(first.hasValue());
    CHECK(first.value().projectId() == project);
    CHECK(first.value().id() == firstDocument);
    CHECK(first.value().objects().empty());
    CHECK(first.value().revisions().at(RevisionScope::Project) == Revision {0U});
    CHECK(first.value().revisions().at(RevisionScope::Document) == Revision {0U});

    auto duplicate = store.addDocument(project, firstDocument);
    REQUIRE_FALSE(duplicate.hasValue());
    CHECK(std::string(duplicate.error().code.value()) == "Document.AlreadyExists");

    auto missing = store.snapshot(makeId<DocumentId>("document.missing"));
    REQUIRE_FALSE(missing.hasValue());
    CHECK(std::string(missing.error().code.value()) == "Document.NotFound");
}

TEST_CASE("DocumentRuntime owns runtime lifecycle while DocumentStore stays internal",
          "[runtime][document][lifecycle]")
{
    AppKernel kernel;
    REQUIRE(kernel.bootstrap().hasValue());

    const auto project = makeId<ProjectId>("project.runtime");
    const auto otherProject = makeId<ProjectId>("project.other");
    const auto document = makeId<DocumentId>("document.runtime");

    auto created = kernel.documentRuntime().create(project, document);
    REQUIRE(created.hasValue());
    CHECK(created.value().state == lasercnc::runtime::DocumentLifecycleState::Open);
    CHECK(kernel.documents().contains(document));

    auto duplicate = kernel.documentRuntime().create(project, document);
    REQUIRE_FALSE(duplicate.hasValue());
    CHECK(std::string(duplicate.error().code.value()) == "Document.LifecycleConflict");

    auto snapshot = kernel.documentRuntime().snapshot(document);
    REQUIRE(snapshot.hasValue());
    CHECK(snapshot.value().projectId() == project);
    CHECK(snapshot.value().id() == document);

    auto detached = kernel.documentRuntime().detach(document);
    REQUIRE(detached.hasValue());
    CHECK(detached.value().state == lasercnc::runtime::DocumentLifecycleState::Detached);
    CHECK_FALSE(kernel.documents().contains(document));

    auto ownershipConflict = kernel.documentRuntime().create(otherProject, document);
    REQUIRE_FALSE(ownershipConflict.hasValue());
    CHECK(std::string(ownershipConflict.error().code.value())
          == "Document.OwnershipConflict");

    const DocumentImage image {project, document, RevisionSet {}, {}};
    auto attached = kernel.documentRuntime().attach(image);
    REQUIRE(attached.hasValue());
    CHECK(attached.value().state == lasercnc::runtime::DocumentLifecycleState::Open);
    REQUIRE(kernel.documentRuntime().close(document).hasValue());
    REQUIRE(kernel.documentRuntime().remove(document).hasValue());
    CHECK(kernel.documentRuntime().list().empty());

    auto missing = kernel.documentRuntime().lifecycle(document);
    REQUIRE_FALSE(missing.hasValue());
    CHECK(std::string(missing.error().code.value()) == "Document.LifecycleNotFound");
    REQUIRE(kernel.shutdown().hasValue());
}

TEST_CASE("DocumentRuntime rejects lifecycle changes outside Ready state",
          "[runtime][document][lifecycle]")
{
    AppKernel kernel;
    const auto project = makeId<ProjectId>("project.runtime.state");
    const auto document = makeId<DocumentId>("document.runtime.state");

    auto beforeReady = kernel.documentRuntime().create(project, document);
    REQUIRE_FALSE(beforeReady.hasValue());
    CHECK(std::string(beforeReady.error().code.value())
          == "Document.RuntimeNotAccepting");

    REQUIRE(kernel.addDocument(project, document).hasValue());
    REQUIRE(kernel.bootstrap().hasValue());
    auto lifecycle = kernel.documentRuntime().lifecycle(document);
    REQUIRE(lifecycle.hasValue());
    CHECK(lifecycle.value().state == lasercnc::runtime::DocumentLifecycleState::Open);
    REQUIRE(kernel.shutdown().hasValue());

    auto afterStop = kernel.documentRuntime().detach(document);
    REQUIRE_FALSE(afterStop.hasValue());
    CHECK(std::string(afterStop.error().code.value())
          == "Document.RuntimeNotAccepting");
}
