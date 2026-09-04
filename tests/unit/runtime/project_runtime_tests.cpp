#include <lasercnc/kernel/app_kernel.hpp>
#include "persistence_fixture.hpp"
#include "fault_injecting_backend.hpp"
#include "kernel_test_module.hpp"
#include <lasercnc/observability/log_service.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

using namespace lasercnc::foundation;
using namespace lasercnc::kernel;
using namespace lasercnc::runtime;
using namespace lasercnc::persistence;
using namespace lasercnc::infrastructure;
using namespace lasercnc::test;

namespace {
template<typename Id> Id id(const char* text) { return Id::create(text).value(); }
std::filesystem::path freshRoot()
{
    static std::atomic_uint sequence{0U};
    const auto tick = std::chrono::system_clock::now().time_since_epoch().count();
    auto root = std::filesystem::path{LCNC_STRESS_TEST_ROOT} / "project-runtime"
        / (std::to_string(tick) + "-" + std::to_string(sequence.fetch_add(1U)));
    std::filesystem::create_directories(root);
    return root;
}
FaultInjectingBackend* configure(AppKernel& kernel, const std::filesystem::path& root)
{
    auto opened = SqlitePersistenceBackend::open({root / "state.db"});
    REQUIRE(opened.hasValue());
    auto backend = std::make_unique<FaultInjectingBackend>(std::move(opened).value());
    auto* observer = backend.get();
    auto snapshots = FilesystemSnapshotStore::create({root / "snapshots", 1024U * 1024U});
    REQUIRE(snapshots.hasValue());
    REQUIRE(kernel.configurePersistence(std::move(backend), std::make_shared<JsonconsAdapter>(),
        std::make_shared<Sha256HashService>(), std::move(snapshots).value()).hasValue());
    return observer;
}
template<typename Host> concept HostStartsProjects = requires(Host& host) { host.projectRuntime().start(); };
template<typename Host> concept HostSkipsProjectClose = requires(Host& host, ProjectId project, DocumentId document) {
    host.documentRuntime().closeForProject(project, document);
};
template<typename Host> concept HostAcquiresProjectLease = requires(Host& host, ProjectId project) {
    host.projectRuntime().acquireActivity(project);
};
static_assert(!HostStartsProjects<AppKernel> && !HostSkipsProjectClose<AppKernel> && !HostAcquiresProjectLease<AppKernel>);
template<typename Host> concept HostOpensWithInternalOwner = requires(Host& host, DocumentId document, ProjectId project) {
    host.documentRuntime().openImpl(document, &project);
};
template<typename Host> concept HostRemovesWithInternalLease = requires(Host& host, DocumentId document, ProjectActivityLease lease) {
    host.documentRuntime().removeImpl(document, lease);
};
static_assert(!HostOpensWithInternalOwner<AppKernel> && !HostRemovesWithInternalLease<AppKernel>);

void configureLifecycleCommands(AppKernel& kernel)
{
    class NullLog final : public lasercnc::observability::ILogService {
    public:
        Result<void> write(const lasercnc::observability::LogRecord&) override { return Result<void>::success(); }
        Result<void> flush() override { return Result<void>::success(); }
    };
    REQUIRE(kernel.executionServices().configure(std::make_shared<JsonconsAdapter>(), std::make_shared<NullLog>()));
    const auto object = Schema::create(id<SchemaId>("schema.lifecycle.object"), {1U, 0U, 0U}, SchemaKind::Object).value();
    const std::array operations{
        std::pair{"kernel.project.create", LifecycleOperation::ProjectCreate},
        std::pair{"kernel.project.open", LifecycleOperation::ProjectOpen},
        std::pair{"kernel.project.close", LifecycleOperation::ProjectClose},
        std::pair{"kernel.document.create", LifecycleOperation::DocumentCreate},
        std::pair{"kernel.document.open", LifecycleOperation::DocumentOpen},
        std::pair{"kernel.document.close", LifecycleOperation::DocumentClose},
        std::pair{"kernel.document.remove", LifecycleOperation::DocumentRemove}};
    REQUIRE(installKernelTestModule(kernel, [&](auto& builder) {
        for(const auto& [name, operation] : operations) {
            CommandDescriptor descriptor{id<CommandName>(name), {1U, 0U, 0U}, object, object,
                ExecutionMode::Synchronous, SideEffectLevel::LifecycleControl, id<CapabilityId>("kernel.lifecycle.control")};
            descriptor.scope = operation == LifecycleOperation::ProjectCreate || operation == LifecycleOperation::ProjectOpen
                || operation == LifecycleOperation::ProjectClose ? ExecutionScope::Project : ExecutionScope::Document;
            descriptor.lifecycleOperation = operation;
            builder.lifecycleCommand(descriptor);
        }
    }));
    const std::array grants{id<CapabilityId>("kernel.lifecycle.control")};
    REQUIRE(kernel.capabilities().replace(id<SessionId>("session.lifecycle"), grants));
}

CommandRequest lifecycleRequest(const char* name, const ProjectId& project, std::optional<DocumentId> document = {})
{
    return {id<RequestId>("request.lifecycle"), {id<SessionId>("session.lifecycle"), project, document},
        id<CommandName>(name), {1U, 0U, 0U}, Value{Value::Object{}}, std::nullopt,
        id<CorrelationId>("correlation.lifecycle"), id<TraceId>("trace.lifecycle")};
}
}

TEST_CASE("Lifecycle commands persist all fixed transitions across fresh Hosts", "[project-runtime][lifecycle-control][persistence]")
{
    const auto root = freshRoot();
    const auto project = id<ProjectId>("project.lifecycle");
    const auto document = id<DocumentId>("document.lifecycle");
    for(unsigned int restart = 0U; restart < 3U; ++restart) {
        CAPTURE(restart);
        AppKernel kernel;
        configure(kernel, root);
        configureLifecycleCommands(kernel);
        REQUIRE(kernel.bootstrap());
        const auto run = [&](const char* name, bool withDocument = false) {
            INFO(name);
            auto result = kernel.execution().executeCommand(lifecycleRequest(name, project,
                withDocument ? std::optional<DocumentId>{document} : std::nullopt));
            if(!result) { INFO(std::string(result.error().code.value())); }
            REQUIRE(result);
            CHECK_FALSE(result.value().commit);
            CHECK(result.value().postExecutionErrors.empty());
        };
        if(restart == 0U) {
            run("kernel.project.create");
            run("kernel.document.create", true);
            run("kernel.document.close", true);
            CHECK_FALSE(kernel.execution().executeCommand(lifecycleRequest("kernel.document.create", project, document)));
            run("kernel.document.open", true);
            run("kernel.project.close");
        } else {
            CHECK(kernel.projectRuntime().lifecycle(project).value().state == ProjectLifecycleState::Closed);
            CHECK_FALSE(kernel.documents().contains(document));
            run("kernel.project.open");
            if(restart == 1U) {
                CHECK(kernel.documentRuntime().lifecycle(document).value().state == DocumentLifecycleState::Detached);
                run("kernel.document.open", true);
                auto snapshot = kernel.documentRuntime().snapshot(document);
                REQUIRE(snapshot);
                CHECK(snapshot.value().revisions().at(lasercnc::state::RevisionScope::Project) == lasercnc::state::Revision{0U});
                run("kernel.document.close", true);
                run("kernel.document.remove", true);
            } else {
                auto records = kernel.persistence().documentCatalog();
                REQUIRE(records);
                REQUIRE(records.value().size() == 1U);
                CHECK(records.value().front().state == DocumentPersistenceState::Removed);
                CHECK_FALSE(kernel.execution().executeCommand(lifecycleRequest("kernel.document.open", project, document)));
                CHECK_FALSE(kernel.execution().executeCommand(lifecycleRequest("kernel.document.create", project, document)));
            }
            run("kernel.project.close");
        }
        auto observer = SqlitePersistenceBackend::open({root / "state.db"});
        REQUIRE(observer);
        auto journal = observer.value()->query("SELECT * FROM state_journal");
        REQUIRE(journal);
        CHECK(journal.value().empty());
        REQUIRE(kernel.shutdown());
    }
}

TEST_CASE("Lifecycle command persistence failure preserves failed state and rejects apparent success", "[project-runtime][lifecycle-control][fault-matrix]")
{
    for(bool throws : {false, true}) {
        const auto root = freshRoot();
        const auto project = id<ProjectId>("project.lifecycle.failure");
        {
            AppKernel kernel;
            auto* backend = configure(kernel, root);
            configureLifecycleCommands(kernel);
            REQUIRE(kernel.bootstrap());
            REQUIRE(kernel.execution().executeCommand(lifecycleRequest("kernel.project.create", project)));
            backend->arm(BackendPoint::Execute, "INSERT INTO project_catalog", 1U, throws);
            auto result = kernel.execution().executeCommand(lifecycleRequest("kernel.project.close", project));
            CHECK_FALSE(result);
            CHECK(backend->hits == 1U);
            CHECK(kernel.projectRuntime().lifecycle(project).value().state == ProjectLifecycleState::Failed);
            CHECK_FALSE(kernel.execution().executeCommand(lifecycleRequest("kernel.project.open", project)));
            REQUIRE(kernel.shutdown());
        }
        AppKernel restarted;
        configure(restarted, root);
        configureLifecycleCommands(restarted);
        REQUIRE(restarted.bootstrap());
        CHECK(restarted.projectRuntime().lifecycle(project).value().state == ProjectLifecycleState::Failed);
        CHECK_FALSE(restarted.execution().executeCommand(lifecycleRequest("kernel.project.open", project)));
        REQUIRE(restarted.shutdown());
    }
}

TEST_CASE("Lifecycle creation reserves identity during authenticated reads and releases rejected reservations", "[project-runtime][lifecycle-control][fault-matrix]")
{
    for(bool throws : {false, true}) {
        AppKernel kernel;
        auto* backend = configure(kernel, freshRoot());
        configureLifecycleCommands(kernel);
        REQUIRE(kernel.bootstrap());
        const auto project = id<ProjectId>("project.lifecycle.identity");
        const auto document = id<DocumentId>("document.lifecycle.identity");
        REQUIRE(kernel.projectRuntime().create(project));
        const auto request = lifecycleRequest("kernel.document.create", project, document);
        backend->arm(BackendPoint::Query, "document_catalog", 1U, throws);
        CHECK_FALSE(kernel.execution().executeCommand(request));
        CHECK(backend->hits == 1U);
        CHECK_FALSE(kernel.documentRuntime().lifecycle(document));
        CHECK_FALSE(kernel.documents().contains(document));
        bool probed = false;
        backend->beforeOperation = [&](BackendPoint point, std::string_view sql) {
            if(probed || point != BackendPoint::Query || sql.find("document_catalog") == std::string_view::npos) { return; }
            probed = true;
            auto reentered = kernel.execution().executeCommand(request);
            REQUIRE_FALSE(reentered);
            CHECK(std::string(reentered.error().code.value()) == "Document.LifecycleConflict");
            CHECK_FALSE(kernel.documentRuntime().remove(document));
            CHECK_FALSE(kernel.projectRuntime().close(project));
        };
        REQUIRE(kernel.execution().executeCommand(request));
        backend->beforeOperation = {};
        CHECK(probed);
        REQUIRE(kernel.execution().executeCommand(lifecycleRequest("kernel.document.close", project, document)));
        REQUIRE(kernel.execution().executeCommand(lifecycleRequest("kernel.document.remove", project, document)));
        const auto before = backend->beginCalls;
        auto reused = kernel.execution().executeCommand(request);
        REQUIRE_FALSE(reused);
        CHECK(std::string(reused.error().code.value()) == "Document.IdentityAlreadyExists");
        CHECK(backend->beginCalls == before);
        CHECK_FALSE(kernel.documentRuntime().lifecycle(document));
        REQUIRE(kernel.shutdown());
    }
}

TEST_CASE("ProjectRuntime owns empty project identity and lifecycle without persistence", "[project-runtime][lifecycle]")
{
    AppKernel kernel;
    const auto project = id<ProjectId>("project.empty");
    CHECK_FALSE(kernel.projectRuntime().create(project));
    REQUIRE(kernel.bootstrap());
    CHECK_FALSE(kernel.projectRuntime().open(project));
    CHECK_FALSE(kernel.projectRuntime().close(project));
    REQUIRE(kernel.projectRuntime().create(project));
    CHECK_FALSE(kernel.projectRuntime().create(project));
    CHECK(kernel.projectRuntime().lifecycle(project).value().state == ProjectLifecycleState::Open);
    CHECK(kernel.documentRuntime().list().empty());
    REQUIRE(kernel.projectRuntime().close(project));
    CHECK(kernel.projectRuntime().list().size() == 1U);
    CHECK_FALSE(kernel.projectRuntime().create(project));
    CHECK_FALSE(kernel.projectRuntime().close(project));
    REQUIRE(kernel.projectRuntime().open(project));
    CHECK_FALSE(kernel.projectRuntime().open(project));
    CHECK_FALSE(kernel.addProject(project));
    REQUIRE(kernel.shutdown());
    CHECK_FALSE(kernel.projectRuntime().close(project));
    CHECK_FALSE(kernel.projectRuntime().accepting());
    CHECK(kernel.projectRuntime().lifecycle(project).value().state == ProjectLifecycleState::Open);
    CHECK(std::string(projectLifecycleStateName(static_cast<ProjectLifecycleState>(255))) == "unknown");
}

TEST_CASE("Kernel admission covers lifecycle persistence until the public operation returns", "[project-runtime][lifecycle][kernel-admission]")
{
    for(unsigned int scenario = 0U; scenario < 8U; ++scenario) {
        DYNAMIC_SECTION("scenario=" << scenario) {
            AppKernel kernel;
            auto* backend = configure(kernel, freshRoot());
            const auto project = id<ProjectId>("project.admission");
            const auto document = id<DocumentId>("document.admission");
            REQUIRE(kernel.bootstrap());
            if(scenario != 0U) { REQUIRE(kernel.projectRuntime().create(project)); }
            if(scenario == 1U) { REQUIRE(kernel.projectRuntime().close(project)); }
            if(scenario >= 5U) { REQUIRE(kernel.documentRuntime().create(project, document)); }
            if(scenario == 5U || scenario == 7U) { REQUIRE(kernel.documentRuntime().close(document)); }
            bool probed = false;
            std::optional<Result<void>> stopped;
            backend->beforeOperation = [&](BackendPoint, std::string_view) {
                if(!probed) {
                    probed = true;
                    stopped.emplace(kernel.shutdown());
                }
            };
            switch(scenario) {
            case 0U: REQUIRE(kernel.projectRuntime().create(project)); break;
            case 1U: REQUIRE(kernel.projectRuntime().open(project)); break;
            case 2U: REQUIRE(kernel.projectRuntime().close(project)); break;
            case 3U: REQUIRE(kernel.documentRuntime().create(project, document)); break;
            case 4U: REQUIRE(kernel.documentRuntime().attach({project, document, {}, {}})); break;
            case 5U: REQUIRE(kernel.documentRuntime().open(document)); break;
            case 6U: REQUIRE(kernel.documentRuntime().close(document)); break;
            case 7U: REQUIRE(kernel.documentRuntime().remove(document)); break;
            }
            backend->beforeOperation = {};
            REQUIRE(probed);
            REQUIRE(stopped.has_value());
            CHECK_FALSE(stopped->hasValue());
            if(!stopped->hasValue()) { CHECK(std::string(stopped->error().code.value()) == "Kernel.ActiveExecutions"); }
            CHECK(kernel.state() == AppKernelState::Ready);
            REQUIRE(kernel.shutdown());
        }
    }
}

TEST_CASE("ProjectRuntime gates document membership and closes only its owned children", "[project-runtime][lifecycle]")
{
    AppKernel kernel;
    const auto project = id<ProjectId>("project.owner");
    const auto other = id<ProjectId>("project.other");
    const auto a = id<DocumentId>("document.a");
    const auto b = id<DocumentId>("document.b");
    const auto c = id<DocumentId>("document.c");
    REQUIRE(kernel.addDocument(other, c));
    REQUIRE(kernel.bootstrap());
    CHECK_FALSE(kernel.documentRuntime().create(project, a));
    CHECK_FALSE(kernel.documentRuntime().attach({project, a, {}, {}}));
    CHECK(kernel.projectRuntime().list().size() == 1U);
    REQUIRE(kernel.projectRuntime().create(project));
    REQUIRE(kernel.documentRuntime().create(project, a));
    REQUIRE(kernel.documentRuntime().attach({project, b, {}, {}}));
    REQUIRE(kernel.projectRuntime().close(project));
    CHECK(kernel.documentRuntime().lifecycle(a).value().state == DocumentLifecycleState::Detached);
    CHECK(kernel.documentRuntime().lifecycle(b).value().state == DocumentLifecycleState::Detached);
    CHECK(kernel.documents().contains(c));
    CHECK_FALSE(kernel.documentRuntime().create(project, a));
    CHECK_FALSE(kernel.documentRuntime().attach({project, a, {}, {}}));
    CHECK_FALSE(kernel.documentRuntime().remove(a));
    CHECK_FALSE(kernel.documentRuntime().snapshot(a));
    REQUIRE(kernel.projectRuntime().open(project));
    CHECK_FALSE(kernel.documents().contains(a));
    REQUIRE(kernel.documentRuntime().attach({project, a, {}, {}}));
    REQUIRE(kernel.projectRuntime().close(project));
    REQUIRE(kernel.shutdown());
}

TEST_CASE("ProjectRuntime persists empty and multi-document containers without automatic child reopen", "[project-runtime][sqlite]")
{
    const auto root = freshRoot();
    const auto empty = id<ProjectId>("project.empty");
    const auto project = id<ProjectId>("project.durable");
    const auto a = id<DocumentId>("document.a");
    const auto b = id<DocumentId>("document.b");
    {
        AppKernel kernel;
        configure(kernel, root);
        REQUIRE(kernel.addProject(empty));
        REQUIRE(kernel.addDocument(project, a));
        REQUIRE(kernel.bootstrap());
        CHECK_FALSE(kernel.persistence().projectCatalogMigrationPending().value());
        REQUIRE(kernel.documentRuntime().create(project, b));
        const auto revisions = kernel.documents().snapshot(a).value().revisions();
        REQUIRE(kernel.projectRuntime().close(project));
        REQUIRE(kernel.projectRuntime().close(empty));
        CHECK(kernel.persistence().recover().value().documents.front().revisions == revisions);
        REQUIRE(kernel.shutdown());
    }
    {
        AppKernel kernel;
        configure(kernel, root);
        REQUIRE(kernel.bootstrap());
        REQUIRE(kernel.projectRuntime().list().size() == 2U);
        CHECK(kernel.projectRuntime().lifecycle(empty).value().state == ProjectLifecycleState::Closed);
        CHECK_FALSE(kernel.documentRuntime().open(a));
        REQUIRE(kernel.projectRuntime().open(project));
        CHECK_FALSE(kernel.documents().contains(a));
        CHECK_FALSE(kernel.documents().contains(b));
        REQUIRE(kernel.documentRuntime().open(a));
        CHECK_FALSE(kernel.documents().contains(b));
        REQUIRE(kernel.projectRuntime().close(project));
        REQUIRE(kernel.projectRuntime().open(empty));
        REQUIRE(kernel.shutdown());
    }
}

TEST_CASE("ProjectRuntime migrates verified legacy roots once and refuses missing project repair", "[project-runtime][migration]")
{
    const auto root = freshRoot();
    const auto project = id<ProjectId>("project.legacy");
    const auto document = id<DocumentId>("document.legacy");
    {
        auto fixture = openPersistenceFixture(root / "state.db");
        REQUIRE(fixture->projectCatalogMigrationPending().value());
        REQUIRE(fixture->saveDocumentLifecycle(project, document, DocumentPersistenceState::Open));
    }
    {
        AppKernel kernel;
        configure(kernel, root);
        REQUIRE(kernel.bootstrap());
        CHECK(kernel.projectRuntime().lifecycle(project).value().state == ProjectLifecycleState::Open);
        CHECK(kernel.documents().contains(document));
        REQUIRE(kernel.shutdown());
    }
    auto observer = SqlitePersistenceBackend::open({root / "state.db"});
    REQUIRE(observer);
    REQUIRE(observer.value()->execute("DELETE FROM project_catalog"));
    AppKernel kernel;
    configure(kernel, root);
    REQUIRE(kernel.addDocument(project, document));
    auto booted = kernel.bootstrap();
    REQUIRE_FALSE(booted);
    const Error* rootCause = &booted.error();
    while(rootCause->cause) { rootCause = rootCause->cause.get(); }
    CHECK(std::string(rootCause->code.value()) == "Project.RecoveryMissingRoot");
    CHECK(kernel.state() == AppKernelState::Failed);
    CHECK_FALSE(kernel.projectRuntime().accepting());
    CHECK(observer.value()->query("SELECT project_id FROM project_catalog").value().empty());
}

TEST_CASE("ProjectRuntime recovers interrupted empty transitions as Failed without rewriting evidence", "[project-runtime][recovery]")
{
    for(const auto state : {ProjectPersistenceState::Opening, ProjectPersistenceState::Closing, ProjectPersistenceState::Failed}) {
        const auto root = freshRoot();
        const auto project = id<ProjectId>("project.interrupted");
        auto fixture = openPersistenceFixture(root / "state.db");
        REQUIRE(fixture->completeProjectCatalogMigration({}));
        REQUIRE(fixture->saveProjectLifecycle(project, state));
        fixture.reset();
        AppKernel kernel;
        configure(kernel, root);
        REQUIRE(kernel.bootstrap());
        auto lifecycle = kernel.projectRuntime().lifecycle(project);
        REQUIRE(lifecycle);
        CHECK(lifecycle.value().state == ProjectLifecycleState::Failed);
        CHECK(lifecycle.value().error.has_value());
        CHECK_FALSE(kernel.projectRuntime().open(project));
        CHECK_FALSE(kernel.projectRuntime().close(project));
        CHECK_FALSE(kernel.documentRuntime().create(project, id<DocumentId>("document.rejected")));
        CHECK(kernel.persistence().projectCatalog().value().front().interruptedTransition == (state != ProjectPersistenceState::Failed));
        REQUIRE(kernel.shutdown());
    }
}

TEST_CASE("ProjectRuntime rejects configured reopen and inconsistent durable child state", "[project-runtime][recovery]")
{
    for(const bool configured : {false, true}) {
        const auto root = freshRoot();
        const auto project = id<ProjectId>("project.closed");
        auto fixture = openPersistenceFixture(root / "state.db");
        REQUIRE(fixture->completeProjectCatalogMigration({}));
        REQUIRE(fixture->saveProjectLifecycle(project, ProjectPersistenceState::Closed));
        if(!configured) {
            REQUIRE(fixture->saveDocumentLifecycle(project, id<DocumentId>("document.invalid"), DocumentPersistenceState::Open));
        }
        fixture.reset();
        AppKernel kernel;
        configure(kernel, root);
        if(configured) { REQUIRE(kernel.addProject(project)); }
        const auto booted = kernel.bootstrap();
        REQUIRE_FALSE(booted);
        const Error* cause = &booted.error();
        while(cause->cause) { cause = cause->cause.get(); }
        CHECK(std::string(cause->code.value()) ==
            (configured ? "Project.RecoveryStateConflict" : "Project.RecoveryChildStateConflict"));
        CHECK(kernel.state() == AppKernelState::Failed);
        CHECK_FALSE(kernel.projectRuntime().accepting());
        CHECK(kernel.persistence().projectCatalog().value().front().state == ProjectPersistenceState::Closed);
    }
}

TEST_CASE("ProjectRuntime retains truthful partial document close on persistence failure", "[project-runtime][sqlite][fault-matrix]")
{
    for(const bool throws : {false, true}) {
        const auto root = freshRoot();
        const auto project = id<ProjectId>("project.partial");
        const auto a = id<DocumentId>("document.a");
        const auto b = id<DocumentId>("document.b");
        {
            AppKernel kernel;
            auto* backend = configure(kernel, root);
            REQUIRE(kernel.addDocument(project, a));
            REQUIRE(kernel.addDocument(project, b));
            REQUIRE(kernel.bootstrap());
            backend->arm(BackendPoint::Execute, "INSERT INTO document_catalog", 3U, throws);
            CHECK_FALSE(kernel.projectRuntime().close(project));
            REQUIRE(backend->hits == 1U);
            CHECK(kernel.projectRuntime().lifecycle(project).value().state == ProjectLifecycleState::Failed);
            CHECK(kernel.documentRuntime().lifecycle(a).value().state == DocumentLifecycleState::Detached);
            CHECK(kernel.documentRuntime().lifecycle(b).value().state == DocumentLifecycleState::Failed);
            CHECK_FALSE(kernel.documents().contains(a));
            CHECK(kernel.documents().contains(b));
            CHECK_FALSE(kernel.documentRuntime().create(project, id<DocumentId>("document.c")));
            REQUIRE(kernel.shutdown());
        }
        AppKernel recovered;
        configure(recovered, root);
        // The second child's failed Closing write leaves its durable state Open.
        // 中文翻译：第二个文档的 Closing 写入失败，持久状态仍为 Open；启动拒绝不一致的父子状态。
        CHECK_FALSE(recovered.bootstrap());
        CHECK_FALSE(recovered.projectRuntime().accepting());
        CHECK(recovered.persistence().projectCatalog().value().front().state == ProjectPersistenceState::Failed);
        CHECK(recovered.persistence().documentCatalog().value().front().state == DocumentPersistenceState::Detached);
    }
}

TEST_CASE("ProjectRuntime fails closed at both durable writes of create open and close", "[project-runtime][sqlite][fault-matrix]")
{
    for(const bool throws : {false, true}) {
        for(unsigned int operation = 0U; operation < 3U; ++operation) {
            for(unsigned int occurrence = 1U; occurrence <= 2U; ++occurrence) {
                INFO("operation=" << operation << " occurrence=" << occurrence << " throws=" << throws);
                const auto root = freshRoot();
                const auto project = id<ProjectId>("project.transition-failure");
                AppKernel kernel;
                auto* backend = configure(kernel, root);
                REQUIRE(kernel.bootstrap());
                if(operation != 0U) { REQUIRE(kernel.projectRuntime().create(project)); }
                if(operation == 1U) { REQUIRE(kernel.projectRuntime().close(project)); }
                backend->arm(BackendPoint::Execute, "INSERT INTO project_catalog", occurrence, throws);
                auto result = operation == 0U ? kernel.projectRuntime().create(project)
                    : operation == 1U ? kernel.projectRuntime().open(project) : kernel.projectRuntime().close(project);
                CHECK_FALSE(result);
                REQUIRE(backend->hits == 1U);
                CHECK(kernel.projectRuntime().lifecycle(project).value().state == ProjectLifecycleState::Failed);
                CHECK(kernel.projectRuntime().lifecycle(project).value().error.has_value());
                CHECK_FALSE(kernel.projectRuntime().create(project));
                CHECK_FALSE(kernel.projectRuntime().open(project));
                CHECK(kernel.persistence().projectCatalog().value().front().state == ProjectPersistenceState::Failed);
                REQUIRE(kernel.shutdown());
            }
        }
    }
}
