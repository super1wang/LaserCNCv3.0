#include "kernel_file_crash_probe.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <utility>

namespace {
thread_local KernelFilePublishProbe publishProbe;

BOOL WINAPI probedMoveFileExW(LPCWSTR source, LPCWSTR target, DWORD flags)
{
    if(publishProbe) { publishProbe(source, target, false); }
    const auto result = ::MoveFileExW(source, target, flags);
    if(result != FALSE && publishProbe) { publishProbe(source, target, true); }
    return result;
}
} // namespace

void setKernelFilePublishProbe(KernelFilePublishProbe probe)
{
    publishProbe = std::move(probe);
}

// Test-only replacement of the static-library object: compile the exact production source,
// intercepting only its rename call. No probe or duplicate implementation enters production.
// 中文翻译：仅在测试中替代静态库对象，编译同一生产源文件并拦截改名调用；生产不含探针或复制实现。
#define MoveFileExW probedMoveFileExW
#include "../../src/infrastructure/persistence/filesystem/windows/filesystem_snapshot_store.cpp"
#undef MoveFileExW
