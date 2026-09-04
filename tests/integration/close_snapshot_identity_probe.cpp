#include <lasercnc/kernel/app_kernel.hpp>
#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>
#include <lasercnc/infrastructure/filesystem_snapshot_store.hpp>
#include <lasercnc/infrastructure/jsoncons_adapter.hpp>
#include <lasercnc/infrastructure/sha256_hash_service.hpp>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

using namespace lasercnc;
int main(int argc, char** argv)
{
    try {
        if(argc != 3) { return 2; }
        const std::filesystem::path root{argv[1]};
        const std::string mode{argv[2]};
        if(mode != "0" && mode != "1" && mode != "2") { return 3; }
        const auto phase = static_cast<std::size_t>(mode.front() - '0');
        kernel::AppKernel host;
        auto backend = infrastructure::SqlitePersistenceBackend::open({root / "state.db"});
        auto snapshots = infrastructure::FilesystemSnapshotStore::create({root / "snapshots", 1024U * 1024U});
        if(!backend || !snapshots) { return 4; }
        auto* observer = backend.value().get();
        if(!host.configurePersistence(std::move(backend).value(), std::make_shared<infrastructure::JsonconsAdapter>(),
            std::make_shared<infrastructure::Sha256HashService>(), std::move(snapshots).value())) { return 6; }
        const auto project = kernel::ProjectId::create("project.close-process").value();
        const std::vector<std::string> identities{
            "document.Case", "document.case", "../中文:CON?", std::string(1024U, 'x')};
        if(phase == 0U) {
            for(const auto& identity : identities) {
                if(!host.addDocument(project, kernel::DocumentId::create(identity).value())) { return 7; }
            }
        }
        if(!host.bootstrap()) { return 8; }
        std::set<std::string> previous;
        auto before = observer->query("SELECT snapshot_id FROM snapshot_index");
        if(!before) { return 5; }
        for(const auto& row : before.value()) {
            const auto* value = row.at("snapshot_id").getIf<std::string>();
            if(value == nullptr) { return 5; }
            previous.insert(*value);
        }
        if(previous.size() != phase * 8U) { return 5; }
        for(const auto& identity : identities) {
            const auto document = kernel::DocumentId::create(identity).value();
            if(phase != 0U) {
                if(host.documents().contains(document) || !host.documentRuntime().open(document)) { return 9; }
            }
            for(int cycle = 0; cycle < 2; ++cycle) {
                auto image = host.documents().snapshot(document);
                if(!image || image.value().id() != document || image.value().projectId() != project) { return 10; }
                if(!host.documentRuntime().close(document)) { return 11; }
                auto stored = host.persistence().latestSnapshot(document);
                if(!stored || !stored.value() || stored.value()->documentId != document
                    || stored.value()->projectId != project || host.documents().contains(document)) { return 12; }
                if(cycle == 0 && !host.documentRuntime().open(document)) { return 13; }
            }
        }
        if(!host.shutdown()) { return 14; }
        std::size_t added = 0U;
        auto after = observer->query("SELECT snapshot_id FROM snapshot_index");
        if(!after || after.value().size() != (phase + 1U) * 8U) { return 16; }
        for(const auto& row : after.value()) {
            const auto* value = row.at("snapshot_id").getIf<std::string>();
            if(value == nullptr) { return 16; }
            const auto& key = *value;
            if(key.size() != 82U || !key.starts_with("snapshot.close.v2.")
                || key.substr(18U).find_first_not_of("0123456789abcdef") != std::string::npos) { return 16; }
            if(!previous.contains(key)) { ++added; std::cout << "new=" << key << '\n'; }
        }
        std::size_t fileCount = 0U;
        for(const auto& entry : std::filesystem::directory_iterator(root / "snapshots")) {
            if(entry.path().extension() != ".snapshot") { return 15; }
            const auto key = entry.path().stem().string();
            if(key.size() != 65U || !key.starts_with("@")
                || key.substr(1U).find_first_not_of("0123456789abcdef") != std::string::npos) { return 16; }
            ++fileCount;
        }
        if(added != 8U || fileCount != after.value().size()) { return 17; }
        std::cout << "close-snapshot-process-verified\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 18;
    }
}
