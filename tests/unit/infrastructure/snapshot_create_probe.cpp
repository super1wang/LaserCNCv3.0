#include "snapshot_create_probe.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <utility>
namespace {
thread_local std::function<void(const std::filesystem::path&)> createProbe;
thread_local std::function<void(const std::filesystem::path&)> publishProbe;
thread_local unsigned int readSharingFailures{0U};
thread_local unsigned int readAttempts{0U};
HANDLE WINAPI probedSnapshotCreateFileW(LPCWSTR path, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES attributes, DWORD disposition, DWORD flags, HANDLE model)
{
    if(disposition == CREATE_NEW && createProbe) { createProbe(path); }
    if(disposition == OPEN_EXISTING && access == GENERIC_READ) {
        ++readAttempts;
        if(readSharingFailures != 0U) {
            --readSharingFailures;
            SetLastError(ERROR_SHARING_VIOLATION);
            return INVALID_HANDLE_VALUE;
        }
    }
    return ::CreateFileW(path, access, share, attributes, disposition, flags, model);
}
BOOL WINAPI probedSnapshotMoveFileExW(LPCWSTR source, LPCWSTR target, DWORD flags)
{
    if(publishProbe) { publishProbe(target); }
    return ::MoveFileExW(source, target, flags);
}
}
void setSnapshotCreateProbe(std::function<void(const std::filesystem::path&)> probe)
{
    createProbe = std::move(probe);
}
void setSnapshotPublishProbe(std::function<void(const std::filesystem::path&)> probe)
{
    publishProbe = std::move(probe);
}
void setSnapshotReadSharingFailures(unsigned int count) { readSharingFailures = count; readAttempts = 0U; }
unsigned int snapshotReadAttempts() { return readAttempts; }
// Compile the exact implementation in the test executable; observe file opens and rename only.
// 中文翻译：测试程序编译同一生产实现，只拦截打开和改名调用；生产无注入入口。
#define CreateFileW probedSnapshotCreateFileW
#define MoveFileExW probedSnapshotMoveFileExW
#include "../../../src/infrastructure/persistence/filesystem/windows/filesystem_snapshot_store.cpp"
#undef MoveFileExW
#undef CreateFileW
