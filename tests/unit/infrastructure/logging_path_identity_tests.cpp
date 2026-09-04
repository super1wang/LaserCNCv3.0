#include <lasercnc/infrastructure/spdlog_log_service.hpp>
#include <catch2/catch_test_macros.hpp>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {
using namespace lasercnc::infrastructure;
std::filesystem::path identityRoot()
{
    static std::atomic_uint sequence{0U};
    auto path = std::filesystem::path{LCNC_STRESS_TEST_ROOT} / "logging-identity"
        / (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
            + "-" + std::to_string(sequence.fetch_add(1U)));
    REQUIRE(std::filesystem::create_directories(path));
    return std::filesystem::absolute(path);
}
void marker(const std::filesystem::path& path)
{
    std::ofstream output(path, std::ios::binary);
    output << "preserved log bytes";
    REQUIRE(output.good());
}
std::string content(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}
SpdlogLogOptions options(const std::filesystem::path& human, const std::filesystem::path& jsonl)
{
    SpdlogLogOptions result;
    result.enableConsole = false;
    result.rotatingFilePath = human;
    result.jsonlFilePath = jsonl;
    result.rotatingFileCount = 2U;
    result.rotatingFileMaxBytes = 128U;
    return result;
}
void rejected(const SpdlogLogOptions& value)
{
    const auto result = SpdlogLogService::create(value);
    CHECK_FALSE(result);
    if(!result) { CHECK(std::string(result.error().code.value()) == "Logging.InvalidOptions"); }
}
} // namespace

TEST_CASE("Logging identity rejects base path aliases before creating outputs", "[logging][path][identity][c6]")
{
    for(const bool existing : {false, true}) {
        for(unsigned int form = 0U; form < 4U; ++form) {
            DYNAMIC_SECTION("existing=" << existing << " form=" << form) {
                const auto root = identityRoot();
                INFO("evidence=" << root.string());
                const auto human = root / "events.log";
                if(existing) { marker(human); }
                auto other = root / "EVENTS.LOG";
                if(form == 1U) { other = std::filesystem::relative(human); }
                if(form == 2U) { other = root / "child" / ".." / "events.log"; }
                if(form == 3U) { other = root / "events.log."; }
                rejected(options(human, other));
                if(existing) { CHECK(content(human) == "preserved log bytes"); }
                else { CHECK(std::filesystem::is_empty(root)); }
            }
        }
    }
}

TEST_CASE("Logging identity rejects base and rotation namespace collisions", "[logging][path][identity][c6]")
{
    for(const bool existing : {false, true}) {
        for(const bool reverse : {false, true}) {
            for(const bool extension : {false, true}) {
                DYNAMIC_SECTION(existing << '-' << reverse << '-' << extension) {
                    const auto root = identityRoot();
                    INFO("evidence=" << root.string());
                    auto first = root / (extension ? "events.log" : "events");
                    auto second = root / (extension ? "events.2.log" : "events.2");
                    if(existing) { marker(first); marker(second); }
                    if(reverse) { std::swap(first, second); }
                    rejected(options(first, second));
                    if(existing) {
                        CHECK(content(first) == "preserved log bytes");
                        CHECK(content(second) == "preserved log bytes");
                    } else { CHECK(std::filesystem::is_empty(root)); }
                }
            }
        }
    }
}

TEST_CASE("Logging identity rejects hard links across bases and rotation files", "[logging][path][identity][c6]")
{
    for(unsigned int form = 0U; form < 4U; ++form) {
        DYNAMIC_SECTION("form=" << form) {
            const auto root = identityRoot();
            INFO("evidence=" << root.string());
            const auto first = root / (form < 2U ? "human.log" : "human.1.log");
            const auto second = root / (form == 0U ? "events.jsonl" : form == 3U ? "human.2.log" : "events.2.jsonl");
            marker(first);
            std::filesystem::create_hard_link(first, second);
            REQUIRE(std::filesystem::equivalent(first, second));
            auto value = options(root / "human.log", root / "events.jsonl");
            if(form == 3U) { value.jsonlFilePath.reset(); }
            rejected(value);
            CHECK(content(first) == "preserved log bytes");
            CHECK(content(second) == "preserved log bytes");
            CHECK(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}) == 2);
        }
    }
}

TEST_CASE("Logging identity resolves aliased parent directories for future outputs", "[logging][path][identity][c6]")
{
    for(const bool nested : {false, true}) {
        DYNAMIC_SECTION("nested=" << nested) {
            const auto root = identityRoot();
            INFO("evidence=" << root.string());
            const auto real = root / "real";
            REQUIRE(std::filesystem::create_directory(real));
            std::filesystem::create_directory_symlink(real, root / "alias");
            const auto tail = nested ? std::filesystem::path{"new/child/events.log"} : std::filesystem::path{"events.log"};
            rejected(options(real / tail, root / "alias" / tail));
            CHECK(std::filesystem::is_empty(real));
        }
    }
}

TEST_CASE("Logging identity preserves distinct case sensitive output names", "[logging][path][identity][c6]")
{
    for(unsigned int scenario = 0U; scenario < 4U; ++scenario) {
        const bool existing = (scenario & 1U) != 0U;
        const bool nested = (scenario & 2U) != 0U;
        DYNAMIC_SECTION("existing=" << existing << " nested=" << nested) {
            const auto parent = identityRoot();
            const auto root = parent / "case-sensitive";
            // Grant the owner full rights only on this new empty test directory.
            // 中文翻译：仅为这个新建空测试目录授予所有者完整权限，不修改工作区或父目录 ACL。
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;FA;;;OW)(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr));
            SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
            const bool made = CreateDirectoryW(root.c_str(), &attributes) != FALSE;
            LocalFree(descriptor);
            REQUIRE(made);
            INFO("evidence=" << root.string());
            const auto handle = CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES
                | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            REQUIRE(handle != INVALID_HANDLE_VALUE);
            FILE_CASE_SENSITIVE_INFO info{FILE_CS_FLAG_CASE_SENSITIVE_DIR};
            const bool enabled = SetFileInformationByHandle(handle, FileCaseSensitiveInfo, &info, sizeof(info)) != FALSE;
            const auto error = GetLastError();
            CloseHandle(handle);
            INFO("case-sensitive setup error=" << error);
            REQUIRE(enabled);
            const auto directory = nested ? root / "new" / "child" : root;
            if(existing && nested) { REQUIRE(std::filesystem::create_directories(directory)); }
            const auto human = directory / "events.log";
            const auto jsonl = directory / "EVENTS.LOG";
            if(existing) { marker(human); marker(jsonl); }
            {
                auto logger = SpdlogLogService::create(options(human, jsonl));
                REQUIRE(logger);
                lasercnc::observability::LogRecord record;
                record.module = "kernel.identity";
                record.category = "test";
                record.message = "distinct outputs";
                REQUIRE(logger.value()->write(record));
                REQUIRE(logger.value()->write(record));
                REQUIRE(logger.value()->flush());
            }
            CHECK_FALSE(std::filesystem::equivalent(human, jsonl));
            CHECK(content(human).find("[info]") != std::string::npos);
            CHECK(content(jsonl).starts_with('{'));
            CHECK(std::filesystem::exists(directory / "events.1.log"));
            CHECK(std::filesystem::exists(directory / "EVENTS.1.LOG"));
        }
    }
}

TEST_CASE("Logging identity permits disjoint files in new directory trees", "[logging][path][identity][c6]")
{
    const auto root = identityRoot();
    INFO("evidence=" << root.string());
    const auto human = root / "human" / "new" / "events.log";
    const auto jsonl = root / "jsonl" / "new" / "events.log";
    {
        auto logger = SpdlogLogService::create(options(human, jsonl));
        REQUIRE(logger);
        lasercnc::observability::LogRecord record;
        record.module = "kernel.identity";
        record.category = "test";
        record.message = "distinct outputs";
        REQUIRE(logger.value()->write(record));
        REQUIRE(logger.value()->write(record));
        REQUIRE(logger.value()->flush());
    }
    CHECK(content(human).find("[info]") != std::string::npos);
    CHECK(content(jsonl).starts_with('{'));
    CHECK(std::filesystem::exists(human.parent_path() / "events.1.log"));
    CHECK(std::filesystem::exists(jsonl.parent_path() / "events.1.log"));
}

TEST_CASE("Logging identity rejects unsupported rotation counts before output", "[logging][path][identity][c6]")
{
    const auto root = identityRoot();
    INFO("evidence=" << root.string());
    auto value = options(root / "human.log", root / "events.jsonl");
    value.rotatingFileCount = 200001U;
    rejected(value);
    CHECK(std::filesystem::is_empty(root));
}

TEST_CASE("Logging identity preflight rejects directory targets before other output", "[logging][path][identity][c6]")
{
    for(const bool humanInvalid : {false, true}) {
        DYNAMIC_SECTION("humanInvalid=" << humanInvalid) {
            const auto root = identityRoot();
            INFO("evidence=" << root.string());
            const auto directory = root / "not-a-file";
            REQUIRE(std::filesystem::create_directory(directory));
            marker(directory / "sentinel");
            const auto value = humanInvalid ? options(directory, root / "new.jsonl") : options(root / "new.log", directory);
            const auto created = SpdlogLogService::create(value);
            CHECK_FALSE(created);
            if(!created) { CHECK(std::string(created.error().code.value()) == "Logging.InitializeFailed"); }
            CHECK(content(directory / "sentinel") == "preserved log bytes");
            CHECK(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}) == 1);
        }
    }
}

TEST_CASE("Logging identity reports later sharing failure without promising IO rollback", "[logging][path][identity][c6]")
{
    for(const bool humanLocked : {false, true}) {
        DYNAMIC_SECTION("humanLocked=" << humanLocked) {
            const auto root = identityRoot();
            INFO("evidence=" << root.string());
            const auto locked = root / "locked.log";
            marker(locked);
            const auto handle = CreateFileW(locked.c_str(), GENERIC_READ, 0U, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            REQUIRE(handle != INVALID_HANDLE_VALUE);
            const auto metadata = CreateFileW(locked.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            REQUIRE(metadata != INVALID_HANDLE_VALUE);
            CloseHandle(metadata);
            const auto value = humanLocked ? options(locked, root / "new.jsonl") : options(root / "new.log", locked);
            const auto created = SpdlogLogService::create(value);
            CHECK_FALSE(created);
            if(!created) { CHECK(std::string(created.error().code.value()) == "Logging.InitializeFailed"); }
            CloseHandle(handle);
            CHECK(content(locked) == "preserved log bytes");
            CHECK(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{})
                == (humanLocked ? 1 : 2));
            if(!humanLocked) { CHECK(content(root / "new.log").empty()); }
        }
    }
}

TEST_CASE("Logging identity follows Windows intermediate directory spelling for aliases", "[logging][path][identity][c6]")
{
    for(const auto* suffix : {L"folder ", L"folder..."}) {
        DYNAMIC_SECTION("suffix length=" << std::wstring{suffix}.size()) {
            const auto root = identityRoot();
            INFO("evidence=" << root.string());
            REQUIRE(std::filesystem::create_directory(root / "folder"));
            const auto nativeRoot = std::filesystem::path{root}.make_preferred().native();
            const auto exact = std::filesystem::path{L"\\\\?\\" + nativeRoot} / suffix;
            REQUIRE(CreateDirectoryW(exact.c_str(), nullptr));
            const auto ordinary = root / suffix;
            marker(exact / "sentinel");
            marker(root / "folder" / "sentinel");
            REQUIRE(std::filesystem::equivalent(ordinary / "sentinel", exact / "sentinel"));
            rejected(options(ordinary / "events.log", exact / "events.log"));
            CHECK(std::distance(std::filesystem::directory_iterator(exact), std::filesystem::directory_iterator{}) == 1);
            CHECK(std::distance(std::filesystem::directory_iterator(root / "folder"), std::filesystem::directory_iterator{}) == 1);
        }
    }
}

TEST_CASE("Logging identity preserves distinct intermediate directory spellings", "[logging][path][identity][c6]")
{
    for(const auto* suffix : {L"folder ", L"folder..."}) {
        DYNAMIC_SECTION("suffix length=" << std::wstring{suffix}.size()) {
            const auto root = identityRoot();
            INFO("evidence=" << root.string());
            const auto plain = root / "folder";
            REQUIRE(std::filesystem::create_directory(plain));
            const auto nativeRoot = std::filesystem::path{root}.make_preferred().native();
            const auto exact = std::filesystem::path{L"\\\\?\\" + nativeRoot} / suffix;
            REQUIRE(CreateDirectoryW(exact.c_str(), nullptr));
            const auto special = root / suffix;
            marker(plain / "sentinel");
            marker(exact / "sentinel");
            REQUIRE_FALSE(std::filesystem::equivalent(plain / "sentinel", special / "sentinel"));
            {
                auto logger = SpdlogLogService::create(options(plain / "events.log", special / "events.log"));
                REQUIRE(logger);
                lasercnc::observability::LogRecord record;
                record.module = "kernel.identity";
                record.category = "test";
                record.message = "distinct directories";
                REQUIRE(logger.value()->write(record));
                REQUIRE(logger.value()->flush());
            }
            CHECK(content(plain / "events.log").find("[info]") != std::string::npos);
            CHECK(content(exact / "events.log").starts_with('{'));
        }
    }
}
