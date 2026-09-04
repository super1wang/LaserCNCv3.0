#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>
#include <lasercnc/infrastructure/filesystem_snapshot_store.hpp>
#include <lasercnc/infrastructure/jsoncons_adapter.hpp>
#include <lasercnc/infrastructure/sha256_hash_service.hpp>
#include <lasercnc/kernel/app_kernel.hpp>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <stdexcept>

namespace {
using namespace lasercnc;

void check(bool accepted, const char* message)
{
    if(!accepted) { throw std::runtime_error(message); }
}

bool releaseRequested()
{
    std::string request;
    const bool received = static_cast<bool>(std::getline(std::cin, request));
    // Windows PowerShell 5 may prefix redirected UTF-8 input with a BOM.
    // 中文翻译：Windows PowerShell 5 重定向输入可能带 BOM，仅在握手首部接受。
    if(request.starts_with("\xEF\xBB\xBF")) { request.erase(0U, 3U); }
    return received && request == "release";
}

int serviceProbe(std::wstring_view mode, const std::filesystem::path& path)
{
    auto observer = infrastructure::SqlitePersistenceBackend::open({path});
    auto backend = infrastructure::SqlitePersistenceBackend::open({path});
    check(observer.hasValue() && backend.hasValue(), "open failed");
    auto serializer = std::make_shared<infrastructure::JsonconsAdapter>();
    auto hashes = std::make_shared<infrastructure::Sha256HashService>();
    if(mode == L"hold-service") {
        persistence::PersistenceService service;
        check(service.configure(std::move(backend).value(), serializer, hashes).hasValue(), "configure failed");
        check(service.initialize().hasValue(), "initialize failed");
        const auto key = kernel::IdempotencyKey::create("key.process.pending").value();
        const foundation::Value signature{"process-host"};
        check(service.claimCommand(key, signature).hasValue(), "claim failed");
        const auto before = observer.value()->query("SELECT * FROM command_idempotency").value();
        check(before.size() == 1U && before.front().at("status") == foundation::Value{"pending"}, "pending missing");
        std::cout << "host-session-ready" << std::endl;
        check(releaseRequested(), "release handshake failed");
        check(observer.value()->query("SELECT * FROM command_idempotency").value() == before, "second Host changed pending claim");
        check(service.releaseCommandClaim(key, signature).hasValue(), "claim release failed");
    } else {
        const auto before = observer.value()->query("SELECT * FROM command_idempotency").value();
        kernel::AppKernel kernel;
        auto store = infrastructure::FilesystemSnapshotStore::create({path.parent_path() / "snapshots"});
        check(store.hasValue(), "snapshot store failed");
        check(kernel.configurePersistence(std::move(backend).value(), serializer, hashes, std::move(store).value()).hasValue(), "configure failed");
        const auto bootstrapped = kernel.bootstrap();
        if(!bootstrapped) {
            check(observer.value()->query("SELECT * FROM command_idempotency").value() == before, "denied Host changed database");
            for(const auto* error = &bootstrapped.error(); error != nullptr; error = error->cause.get()) {
                std::cerr << error->code.value() << '\n';
                if(error->code.value() == "Persistence.HostAlreadyOwned") { return 23; }
            }
            return 4;
        }
        const auto after = observer.value()->query("SELECT * FROM command_idempotency").value();
        check(after.size() == before.size(), "takeover changed claim count");
        if(!before.empty()) {
            check(before.front().at("status") == foundation::Value{"pending"}, "crash evidence missing");
            check(after.front().at("status") == foundation::Value{"abandoned"}, "legitimate takeover did not recover claim");
        }
        check(kernel.persistence().sessionStatus().ready, "admission diagnostics not ready");
        check(kernel.shutdown().hasValue(), "shutdown failed");
    }
    std::cout << "host-session-acquired" << std::endl;
    return 0;
}
} // namespace

// This is a test-only process probe, not a product CLI.
// 中文翻译：仅用于所有权进程测试，不是产品 CLI。
int wmain(int argc, wchar_t** argv)
{
    if(argc != 3) { return 2; }
    if(std::wstring_view{argv[1]} == L"hold-service" || std::wstring_view{argv[1]} == L"probe-service") {
        try { return serviceProbe(argv[1], std::filesystem::path{argv[2]}); }
        catch(const std::exception& error) { std::cerr << error.what(); return 6; }
    }
    auto opened = lasercnc::infrastructure::SqlitePersistenceBackend::open({std::filesystem::path{argv[2]}});
    if(!opened) { std::cerr << opened.error().message; return 3; }
    auto admitted = opened.value()->acquireHostSession();
    if(!admitted) {
        std::cerr << admitted.error().code.value() << '\n';
        return admitted.error().code.value() == "Persistence.HostAlreadyOwned" ? 23 : 4;
    }
    if(std::wstring_view{argv[1]} == L"hold") {
        std::cout << "host-session-ready" << std::endl;
        std::string request;
        const bool received = static_cast<bool>(std::getline(std::cin, request));
        // Windows PowerShell 5 may prefix its redirected UTF-8 input with a BOM.
        // 中文翻译：Windows PowerShell 5 的重定向 UTF-8 输入可能带 BOM，仅在测试握手首部接受。
        if(request.starts_with("\xEF\xBB\xBF")) { request.erase(0U, 3U); }
        if(!received || request != "release") {
            std::cerr << "release-handshake-invalid bytes=" << request.size();
            for(const unsigned char byte : request) { std::cerr << ' ' << static_cast<unsigned int>(byte); }
            return 5;
        }
    } else if(std::wstring_view{argv[1]} != L"probe") {
        return 2;
    }
    std::cout << "host-session-acquired" << std::endl;
    return 0;
}
