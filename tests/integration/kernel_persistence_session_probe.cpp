#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

// This is a test-only process probe, not a product CLI.
// 中文翻译：仅用于所有权进程测试，不是产品 CLI。
int wmain(int argc, wchar_t** argv)
{
    if(argc != 3) { return 2; }
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
