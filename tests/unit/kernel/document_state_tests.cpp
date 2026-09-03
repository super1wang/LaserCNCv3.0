#include <lasercnc/state/document_store.hpp>
#include <lasercnc/state/revision.hpp>

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
