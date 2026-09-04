#include <lasercnc/kernel/app_kernel.hpp>
#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>
#include <lasercnc/infrastructure/filesystem_snapshot_store.hpp>
#include <lasercnc/infrastructure/jsoncons_adapter.hpp>
#include <lasercnc/infrastructure/sha256_hash_service.hpp>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string_view>

using namespace lasercnc;
int main(int argc, char** argv)
{
    try {
        if(argc != 3) { return 2; }
        const std::filesystem::path root{argv[1]};
        const std::string_view mode{argv[2]};
        if(mode != "seed" && mode != "read") { return 3; }
        kernel::AppKernel host;
        auto backend = infrastructure::SqlitePersistenceBackend::open({root / "state.db"});
        auto snapshots = infrastructure::FilesystemSnapshotStore::create({root / "snapshots", 1024U * 1024U});
        if(!backend || !snapshots) { return 4; }
        if(!host.configurePersistence(std::move(backend).value(), std::make_shared<infrastructure::JsonconsAdapter>(),
            std::make_shared<infrastructure::Sha256HashService>(), std::move(snapshots).value())) { return 5; }
        const auto project = kernel::ProjectId::create("project.catalog-process").value();
        const auto document = kernel::DocumentId::create("document.catalog-process").value();
        if(mode == "seed" && !host.addDocument(project, document)) { return 6; }
        if(!host.bootstrap()) { return 7; }
        if(mode == "seed" && !host.projectRuntime().close(project)) { return 8; }
        auto projects = host.projectRuntime().catalog(project);
        auto documents = host.documentRuntime().catalog(project);
        if(!projects || !documents || projects.value().entries.size() != 1U || documents.value().entries.size() != 1U) { return 9; }
        if(projects.value().entries.front().state != runtime::ProjectLifecycleState::Closed
           || documents.value().entries.front().state != runtime::DocumentLifecycleState::Detached
           || host.scheduler().activeTaskCount() != 0U || host.documents().contains(document)) { return 10; }
        std::cout << "project=" << std::hex << std::setfill('0');
        for(const auto part : projects.value().version.epoch) { std::cout << std::setw(8) << part; }
        std::cout << "\ndocument=";
        for(const auto part : documents.value().version.epoch) { std::cout << std::setw(8) << part; }
        std::cout << "\nclosed-detached-no-execution\n";
        return host.shutdown() ? 0 : 11;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 12;
    }
}
