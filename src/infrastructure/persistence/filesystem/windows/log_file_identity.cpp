#include "log_file_identity.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <limits>
#include <system_error>
#include <utility>

namespace lasercnc::infrastructure::detail {
namespace {
class Handle final {
public:
    explicit Handle(HANDLE value) noexcept : value_(value) {}
    ~Handle() { if(value_ != INVALID_HANDLE_VALUE) { CloseHandle(value_); } }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const noexcept { return value_; }
private:
    HANDLE value_;
};
[[noreturn]] void inspectionFailure(const char* operation, DWORD error = GetLastError())
{
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}
bool missing(DWORD error) noexcept
{
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}
HANDLE openMetadata(const std::filesystem::path& path)
{
    return CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}
FileIdentity identity(HANDLE handle, bool directory)
{
    FILE_STANDARD_INFO standard{};
    FILE_ID_INFO id{};
    if(GetFileType(handle) != FILE_TYPE_DISK) { inspectionFailure("Log target is not a disk file", ERROR_INVALID_NAME); }
    if(!GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard))
        || !GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id))) {
        inspectionFailure("Cannot inspect log file identity");
    }
    if((standard.Directory != FALSE) != directory) {
        inspectionFailure("Log target has the wrong file type", ERROR_DIRECTORY);
    }
    FileIdentity result{id.VolumeSerialNumber, {}};
    std::copy(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), result.identifier.begin());
    return result;
}

std::filesystem::path normalizedTarget(const std::filesystem::path& original)
{
    // Extended paths bypass Win32 normalization; ordinary paths use its actual spelling rules.
    // 中文翻译：扩展路径跳过 Win32 规范化；普通路径按实际 Win32 名称规则处理。
    if(original.native().starts_with(L"\\\\?\\")) { return original; }
    const auto required = GetFullPathNameW(original.c_str(), 0U, nullptr, nullptr);
    if(required == 0U) { inspectionFailure("Cannot resolve log path"); }
    std::wstring full(required, L'\0');
    const auto length = GetFullPathNameW(original.c_str(), required, full.data(), nullptr);
    if(length == 0U || length >= required) { inspectionFailure("Cannot resolve stable log path"); }
    full.resize(length);
    // Do not trim intermediate components: Windows preserves some trailing spaces/dots there.
    // 中文翻译：不得手工裁剪中间目录组件；Windows 会保留其中某些尾随空格和点。
    return std::filesystem::path{std::move(full)};
}

TargetName prospectiveName(const std::filesystem::path& target)
{
    auto parent = target.parent_path();
    // Pin the already-normalized directory spelling before it becomes the final component.
    // 中文翻译：目录变成查询路径的末组件之前固定已规范化的拼写，避免第二次规范化改查另一目录。
    const auto nativeParent = parent.native();
    if(!nativeParent.starts_with(L"\\\\?\\") && !nativeParent.starts_with(L"\\\\.\\")) {
        parent = nativeParent.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + nativeParent.substr(2U)
            : L"\\\\?\\" + nativeParent;
    }
    auto relative = target.filename().native();
    // Alternate streams share a file identity and are not independent log files.
    // 中文翻译：备用数据流共享文件身份，不作为相互独立的日志文件。
    relative = relative.substr(0U, relative.find(L':'));
    while(!parent.empty()) {
        const Handle handle{openMetadata(parent)};
        if(handle.get() != INVALID_HANDLE_VALUE) {
            const auto id = identity(handle.get(), true);
            FILE_CASE_SENSITIVE_INFO info{};
            if(!GetFileInformationByHandleEx(handle.get(), FileCaseSensitiveInfo, &info, sizeof(info))) {
                inspectionFailure("Cannot inspect log directory case policy");
            }
            if(relative.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                inspectionFailure("Log relative name exceeds comparison limits", ERROR_FILENAME_EXCED_RANGE);
            }
            return {id, std::move(relative), (info.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR) != 0U};
        }
        const auto error = GetLastError();
        if(!missing(error)) { inspectionFailure("Cannot inspect log parent directory", error); }
        const auto next = parent.parent_path();
        if(next == parent) { break; }
        relative = parent.filename().native() + L'\\' + relative;
        parent = next;
    }
    inspectionFailure("Log path has no existing directory anchor", ERROR_PATH_NOT_FOUND);
}
} // namespace

bool NameLess::operator()(const TargetName& left, const TargetName& right) const
{
    if(left.directory != right.directory) { return left.directory < right.directory; }
    if(left.caseSensitive != right.caseSensitive) {
        inspectionFailure("Log directory case policy changed during admission", ERROR_RETRY);
    }
    const auto compared = CompareStringOrdinal(left.relative.data(), static_cast<int>(left.relative.size()),
        right.relative.data(), static_cast<int>(right.relative.size()), left.caseSensitive ? FALSE : TRUE);
    if(compared == 0) { inspectionFailure("Cannot compare log target names"); }
    return compared == CSTR_LESS_THAN;
}

InspectedLogFile inspectLogFileTarget(const std::filesystem::path& path)
{
    const auto target = normalizedTarget(path);
    auto name = prospectiveName(target);
    const Handle handle{openMetadata(target)};
    if(handle.get() != INVALID_HANDLE_VALUE) {
        return {std::move(name), identity(handle.get(), false)};
    }
    const auto error = GetLastError();
    if(!missing(error)) { inspectionFailure("Cannot inspect existing log target", error); }
    return {std::move(name), std::nullopt};
}
} // namespace lasercnc::infrastructure::detail
