#include <lasercnc/infrastructure/filesystem_snapshot_store.hpp>
#include "../../../file_path_validation.hpp"

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/infrastructure/sha256_hash_service.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace lasercnc::infrastructure {
namespace {

std::atomic_ullong temporarySequence{0U};
constexpr std::string_view envelopeMagic{"LCNCSN02"};
constexpr std::size_t maximumIdentityBytes = 4096U;
constexpr std::size_t maximumEnvelopeOverhead = 12U + maximumIdentityBytes;

std::string pathToUtf8(const std::filesystem::path& path)
{
    const auto encoded = path.u8string();
    return {
        reinterpret_cast<const char*>(encoded.data()),
        encoded.size()};
}

foundation::Error snapshotError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const std::filesystem::path& path,
    std::optional<std::uint32_t> systemCode = std::nullopt)
{
    foundation::Value::Object details {
        {"path", foundation::Value {pathToUtf8(path)}},
    };
    if(systemCode.has_value()) {
        details.emplace(
            "systemCode",
            foundation::Value {static_cast<std::int64_t>(*systemCode)});
    }
    return foundation::makeError(
        code, category, message, foundation::Value {std::move(details)});
}

bool safeLegacyId(std::string_view value)
{
    if(value.empty() || value == "." || value == ".." || value.size() > 128U) {
        return false;
    }
    if(!std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9') || character == '.' || character == '_'
            || character == '-';
    })) { return false; }
    std::string base(value.substr(0U, value.find('.')));
    for(auto& character : base) {
        if(character >= 'a' && character <= 'z') { character = static_cast<char>(character - 'a' + 'A'); }
    }
    return base != "CON" && base != "PRN" && base != "AUX" && base != "NUL"
        && !(base.size() == 4U && (base.starts_with("COM") || base.starts_with("LPT"))
            && base[3] >= '1' && base[3] <= '9');
}

class FileHandle final {
public:
    explicit FileHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}
    ~FileHandle() { reset(); }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

    void reset(HANDLE replacement = INVALID_HANDLE_VALUE) noexcept
    {
        if(valid()) {
            static_cast<void>(CloseHandle(handle_));
        }
        handle_ = replacement;
    }

private:
    HANDLE handle_;
};

foundation::Result<void> writeAll(HANDLE file, std::string_view payload)
{
    auto remaining = payload;
    while(!remaining.empty()) {
        const auto chunkSize = std::min<std::size_t>(
            remaining.size(), static_cast<std::size_t>(std::numeric_limits<DWORD>::max()));
        DWORD written = 0U;
        if(WriteFile(
               file,
               remaining.data(),
               static_cast<DWORD>(chunkSize),
               &written,
               nullptr)
               == FALSE
           || written != chunkSize) {
            return foundation::Result<void>::failure(foundation::makeError(
                "Snapshot.WriteFailed",
                foundation::ErrorCategory::Infrastructure,
                "The snapshot payload could not be written",
                foundation::Value {foundation::Value::Object {
                    {"systemCode", foundation::Value {
                        static_cast<std::int64_t>(GetLastError())}},
                }}));
        }
        remaining.remove_prefix(written);
    }
    return foundation::Result<void>::success();
}

foundation::Result<std::string> readFile(
    const std::filesystem::path& path,
    std::size_t maximumPayloadBytes,
    HANDLE borrowed = INVALID_HANDLE_VALUE)
{
    FileHandle file;
    DWORD openError = ERROR_SUCCESS;
    if(borrowed == INVALID_HANDLE_VALUE) {
        // Another immutable publisher may briefly hold an incompatible rename handle.
        // 中文翻译：另一个不可变发布者可能短暂持有不兼容的改名句柄；仅对共享冲突有限重试。
        for(unsigned int attempt = 0U; attempt < 8U; ++attempt) {
            file.reset(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if(file.valid()) { break; }
            openError = GetLastError();
            if(openError != ERROR_SHARING_VIOLATION || attempt == 7U) { break; }
            Sleep(1U);
        }
    }
    const auto handle = borrowed != INVALID_HANDLE_VALUE ? borrowed : file.get();
    if(handle == INVALID_HANDLE_VALUE) {
        const auto error = openError;
        return foundation::Result<std::string>::failure(snapshotError(
            error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                ? "Snapshot.NotFound"
                : "Snapshot.ReadFailed",
            error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                ? foundation::ErrorCategory::NotFound
                : foundation::ErrorCategory::Infrastructure,
            "The snapshot payload could not be opened",
            path,
            error));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if(GetFileInformationByHandle(handle, &information) == FALSE
        || (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U
        || information.nNumberOfLinks != 1U || GetFileType(handle) != FILE_TYPE_DISK) {
        return foundation::Result<std::string>::failure(snapshotError(
            "Snapshot.InvalidStorageFile", foundation::ErrorCategory::Infrastructure,
            "A snapshot must be a regular non-reparse file with one link", path));
    }
    LARGE_INTEGER size {};
    if(GetFileSizeEx(handle, &size) == FALSE || size.QuadPart < 0
       || static_cast<unsigned long long>(size.QuadPart) > maximumPayloadBytes) {
        return foundation::Result<std::string>::failure(snapshotError(
            "Snapshot.PayloadTooLarge",
            foundation::ErrorCategory::Validation,
            "The snapshot payload exceeds the configured size limit",
            path,
            GetLastError()));
    }
    std::string payload(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset = 0U;
    while(offset < payload.size()) {
        const auto chunkSize = std::min<std::size_t>(
            payload.size() - offset,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max()));
        DWORD read = 0U;
        if(ReadFile(
               handle,
               payload.data() + offset,
               static_cast<DWORD>(chunkSize),
               &read,
               nullptr)
               == FALSE
           || read == 0U) {
            return foundation::Result<std::string>::failure(snapshotError(
                "Snapshot.ReadFailed",
                foundation::ErrorCategory::Infrastructure,
                "The snapshot payload could not be read completely",
                path,
                GetLastError()));
        }
        offset += read;
    }
    return foundation::Result<std::string>::success(std::move(payload));
}

foundation::Result<bool> exactFile(const std::filesystem::path& path, bool legacy)
{
    WIN32_FIND_DATAW data{};
    const auto handle = FindFirstFileW(path.c_str(), &data);
    if(handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return foundation::Result<bool>::success(false);
        }
        return foundation::Result<bool>::failure(snapshotError("Snapshot.ReadFailed",
            foundation::ErrorCategory::Infrastructure, "The snapshot destination could not be inspected", path, error));
    }
    static_cast<void>(FindClose(handle));
    if(path.filename().native() != data.cFileName) {
        if(legacy) { return foundation::Result<bool>::success(false); }
        return foundation::Result<bool>::failure(snapshotError("Snapshot.InvalidStorageFile",
            foundation::ErrorCategory::Conflict, "The encoded snapshot filename is not canonical", path));
    }
    if((data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        return foundation::Result<bool>::failure(snapshotError("Snapshot.InvalidStorageFile",
            foundation::ErrorCategory::Infrastructure, "The snapshot destination is not a regular file", path));
    }
    return foundation::Result<bool>::success(true);
}

std::string envelope(const kernel::SnapshotId& id, std::string_view payload)
{
    std::string result(envelopeMagic);
    const auto length = static_cast<std::uint32_t>(id.value().size());
    for(const auto shift : {24U, 16U, 8U, 0U}) {
        result.push_back(static_cast<char>((length >> shift) & 0xffU));
    }
    result.append(id.value());
    result.append(payload);
    return result;
}

foundation::Result<std::string> decodeEnvelope(std::string stored, const kernel::SnapshotId& id,
    std::size_t limit, const std::filesystem::path& path)
{
    if(stored.size() < 12U || !stored.starts_with(envelopeMagic)) {
        return foundation::Result<std::string>::failure(snapshotError("Snapshot.InvalidEnvelope",
            foundation::ErrorCategory::Infrastructure, "The snapshot file envelope is truncated or unsupported", path));
    }
    std::uint32_t length = 0U;
    for(std::size_t index = 8U; index < 12U; ++index) {
        length = (length << 8U) | static_cast<unsigned char>(stored[index]);
    }
    if(length == 0U || length > maximumIdentityBytes || stored.size() - 12U < length) {
        return foundation::Result<std::string>::failure(snapshotError("Snapshot.InvalidEnvelope",
            foundation::ErrorCategory::Infrastructure, "The snapshot identity length is invalid", path));
    }
    if(std::string_view(stored).substr(12U, length) != id.value()) {
        return foundation::Result<std::string>::failure(snapshotError("Snapshot.IdentityMismatch",
            foundation::ErrorCategory::Conflict, "The snapshot file is bound to another exact identity", path));
    }
    if(stored.size() - 12U - length > limit) {
        return foundation::Result<std::string>::failure(snapshotError("Snapshot.PayloadTooLarge",
            foundation::ErrorCategory::Validation, "The snapshot payload exceeds the configured size limit", path));
    }
    stored.erase(0U, 12U + length);
    return foundation::Result<std::string>::success(std::move(stored));
}

} // namespace

class FilesystemSnapshotStore::Impl final {
public:
    Impl(std::filesystem::path directory, std::size_t maximumPayloadBytes)
        : directory_(std::move(directory)), maximumPayloadBytes_(maximumPayloadBytes)
    {
    }

    struct Location final { std::filesystem::path path; bool exists; bool encoded; };

    foundation::Result<Location> locate(
        const kernel::SnapshotId& snapshotId) const
    {
        if(snapshotId.value().empty() || snapshotId.value().size() > maximumIdentityBytes) {
            return foundation::Result<Location>::failure(snapshotError(
                "Snapshot.InvalidIdForStore",
                foundation::ErrorCategory::Validation,
                "The snapshot identity exceeds the file store byte budget",
                directory_));
        }
        const auto identity = snapshotId.value();
        auto digest = Sha256HashService{}.digest(std::as_bytes(std::span{identity.data(), identity.size()}));
        if(!digest) { return foundation::Result<Location>::failure(std::move(digest).error()); }
        auto encoded = directory_ / ("@" + std::string(digest.value().value().substr(7U)) + ".snapshot");
        auto encodedExists = exactFile(encoded, false);
        if(!encodedExists) { return foundation::Result<Location>::failure(std::move(encodedExists).error()); }
        if(safeLegacyId(identity)) {
            auto legacy = directory_ / (std::string(identity) + ".snapshot");
            auto legacyExists = exactFile(legacy, true);
            if(!legacyExists) { return foundation::Result<Location>::failure(std::move(legacyExists).error()); }
            if(legacyExists.value()) {
                if(encodedExists.value()) {
                    return foundation::Result<Location>::failure(snapshotError("Snapshot.AmbiguousStorage",
                        foundation::ErrorCategory::Conflict, "Both legacy and encoded files claim the same snapshot", directory_));
                }
                return foundation::Result<Location>::success({std::move(legacy), true, false});
            }
        }
        return foundation::Result<Location>::success({std::move(encoded), encodedExists.value(), true});
    }

    foundation::Result<std::string> read(const Location& location, const kernel::SnapshotId& id,
        HANDLE borrowed = INVALID_HANDLE_VALUE) const
    {
        auto stored = readFile(location.path,
            maximumPayloadBytes_ + (location.encoded ? maximumEnvelopeOverhead : 0U), borrowed);
        if(!stored || !location.encoded) { return stored; }
        return decodeEnvelope(std::move(stored).value(), id, maximumPayloadBytes_, location.path);
    }

    std::filesystem::path directory_;
    std::size_t maximumPayloadBytes_;
    mutable std::mutex mutex_;
};

FilesystemSnapshotStore::FilesystemSnapshotStore(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation))
{
}

FilesystemSnapshotStore::~FilesystemSnapshotStore() = default;

foundation::Result<std::unique_ptr<FilesystemSnapshotStore>>
FilesystemSnapshotStore::create(FilesystemSnapshotStoreOptions options)
{
    if(options.directory.empty() || detail::containsEmbeddedNull(options.directory) || options.maximumPayloadBytes == 0U
        || options.maximumPayloadBytes > std::numeric_limits<std::size_t>::max() - maximumEnvelopeOverhead) {
        return foundation::Result<std::unique_ptr<FilesystemSnapshotStore>>::failure(
            snapshotError(
                "Snapshot.InvalidStoreOptions",
                foundation::ErrorCategory::Validation,
                "The snapshot store requires a non-empty directory without null characters and a positive size limit",
                options.directory));
    }
    for(const auto& component : options.directory.relative_path()) {
        const auto part = component.native();
        if(part != L"." && part != L".." && !part.empty() && (part.back() == L'.' || part.back() == L' ')) {
            return foundation::Result<std::unique_ptr<FilesystemSnapshotStore>>::failure(snapshotError(
                "Snapshot.InvalidStoreOptions", foundation::ErrorCategory::Validation,
                "Snapshot directory components cannot have ambiguous trailing dots or spaces", options.directory));
        }
    }
    std::error_code error;
    auto directory = std::filesystem::absolute(options.directory, error).lexically_normal();
    if(error) {
        return foundation::Result<std::unique_ptr<FilesystemSnapshotStore>>::failure(
            snapshotError(
                "Snapshot.StoreInitializationFailed",
                foundation::ErrorCategory::Infrastructure,
                "The snapshot directory could not be resolved",
                options.directory,
                static_cast<std::uint32_t>(error.value())));
    }
    // Use the native extended path form, independent of process longPathAware manifest flags.
    // 中文翻译：使用原生长路径形式，不依赖宿主清单的 longPathAware 开关。
    const auto native = directory.native();
    if(!native.starts_with(L"\\\\?\\")) {
        directory = native.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + native.substr(2U) : L"\\\\?\\" + native;
    }
    std::filesystem::create_directories(directory, error);
    if(error || !std::filesystem::is_directory(directory, error) || error) {
        return foundation::Result<std::unique_ptr<FilesystemSnapshotStore>>::failure(
            snapshotError(
                "Snapshot.StoreInitializationFailed",
                foundation::ErrorCategory::Infrastructure,
                "The snapshot directory could not be created",
                directory,
                static_cast<std::uint32_t>(error.value())));
    }
    return foundation::Result<std::unique_ptr<FilesystemSnapshotStore>>::success(
        std::unique_ptr<FilesystemSnapshotStore>(new FilesystemSnapshotStore(
            std::make_unique<Impl>(std::move(directory), options.maximumPayloadBytes))));
}

foundation::Result<platform::SnapshotWriteDisposition>
FilesystemSnapshotStore::writeAtomically(
    const kernel::SnapshotId& snapshotId,
    std::string_view payload)
{
    std::lock_guard lock(implementation_->mutex_);
    auto target = implementation_->locate(snapshotId);
    if(!target) {
        return foundation::Result<platform::SnapshotWriteDisposition>::failure(
            std::move(target).error());
    }
    if(payload.size() > implementation_->maximumPayloadBytes_) {
        return foundation::Result<platform::SnapshotWriteDisposition>::failure(snapshotError(
            "Snapshot.PayloadTooLarge",
            foundation::ErrorCategory::Validation,
            "The snapshot payload exceeds the configured size limit",
            target.value().path));
    }
    if(target.value().exists) {
        auto existing = implementation_->read(target.value(), snapshotId);
        if(!existing) {
            return foundation::Result<platform::SnapshotWriteDisposition>::failure(
                std::move(existing).error());
        }
        if(existing.value() != payload) {
            return foundation::Result<platform::SnapshotWriteDisposition>::failure(snapshotError(
                "Snapshot.IdentityConflict",
                foundation::ErrorCategory::Conflict,
                "A snapshot identity is already bound to different content",
                target.value().path));
        }
        return foundation::Result<platform::SnapshotWriteDisposition>::success(
            platform::SnapshotWriteDisposition::AlreadyPresent);
    }

    const auto stored = envelope(snapshotId, payload);
    std::wstring temporary;
    FileHandle file;
    DWORD createError = ERROR_SUCCESS;
    for(unsigned int attempt = 0U; attempt < 8U; ++attempt) {
        temporary = target.value().path.wstring() + L".tmp."
            + std::to_wstring(GetCurrentProcessId()) + L'.' + std::to_wstring(GetTickCount64()) + L'.'
            + std::to_wstring(temporarySequence.fetch_add(1U));
        file.reset(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0U, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr));
        if(file.valid()) { break; }
        createError = GetLastError();
        if(createError != ERROR_FILE_EXISTS && createError != ERROR_ALREADY_EXISTS) { break; }
    }
    if(!file.valid()) {
        return foundation::Result<platform::SnapshotWriteDisposition>::failure(snapshotError(
            createError == ERROR_FILE_EXISTS || createError == ERROR_ALREADY_EXISTS
                ? "Snapshot.TemporaryCollision" : "Snapshot.WriteFailed",
            foundation::ErrorCategory::Infrastructure,
            "The temporary snapshot file could not be created",
            temporary,
            createError));
    }
    auto written = writeAll(file.get(), stored);
    if(!written || FlushFileBuffers(file.get()) == FALSE) {
        const auto error = written ? GetLastError() : ERROR_WRITE_FAULT;
        file.reset();
        static_cast<void>(DeleteFileW(temporary.c_str()));
        return foundation::Result<platform::SnapshotWriteDisposition>::failure(
            written ? snapshotError(
                "Snapshot.FlushFailed",
                foundation::ErrorCategory::Infrastructure,
                "The temporary snapshot file could not be flushed",
                temporary,
                error)
                    : std::move(written).error());
    }
    file.reset();
    if(MoveFileExW(
           temporary.c_str(), target.value().path.c_str(), MOVEFILE_WRITE_THROUGH)
       == FALSE) {
        const auto error = GetLastError();
        static_cast<void>(DeleteFileW(temporary.c_str()));
        if(error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
            auto existing = implementation_->read(target.value(), snapshotId);
            if(existing && existing.value() == payload) {
                return foundation::Result<platform::SnapshotWriteDisposition>::success(
                    platform::SnapshotWriteDisposition::AlreadyPresent);
            }
            if(existing) {
                return foundation::Result<platform::SnapshotWriteDisposition>::failure(
                    snapshotError(
                        "Snapshot.IdentityConflict",
                        foundation::ErrorCategory::Conflict,
                        "A snapshot identity is already bound to different content",
                        target.value().path));
            }
            return foundation::Result<platform::SnapshotWriteDisposition>::failure(
                std::move(existing).error());
        }
        return foundation::Result<platform::SnapshotWriteDisposition>::failure(snapshotError(
            "Snapshot.PublishFailed",
            foundation::ErrorCategory::Infrastructure,
            "The temporary snapshot file could not be published atomically",
            target.value().path,
            error));
    }
    return foundation::Result<platform::SnapshotWriteDisposition>::success(
        platform::SnapshotWriteDisposition::Created);
}

foundation::Result<std::string> FilesystemSnapshotStore::read(
    const kernel::SnapshotId& snapshotId) const
{
    std::lock_guard lock(implementation_->mutex_);
    auto path = implementation_->locate(snapshotId);
    if(!path) {
        return foundation::Result<std::string>::failure(std::move(path).error());
    }
    return implementation_->read(path.value(), snapshotId);
}

foundation::Result<bool> FilesystemSnapshotStore::remove(
    const kernel::SnapshotId& snapshotId)
{
    std::lock_guard lock(implementation_->mutex_);
    auto path = implementation_->locate(snapshotId);
    if(!path) {
        return foundation::Result<bool>::failure(std::move(path).error());
    }
    if(!path.value().exists) { return foundation::Result<bool>::success(false); }
    FileHandle file(CreateFileW(path.value().path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if(!file.valid()) {
        const auto error = GetLastError();
        if(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return foundation::Result<bool>::success(false);
        }
        return foundation::Result<bool>::failure(snapshotError("Snapshot.RemoveFailed",
            foundation::ErrorCategory::Infrastructure, "The snapshot could not be opened for deletion", path.value().path, error));
    }
    auto verified = implementation_->read(path.value(), snapshotId, file.get());
    if(!verified) { return foundation::Result<bool>::failure(std::move(verified).error()); }
    // Delete the verified handle, not a path that could resolve to a different file after closing it.
    // 中文翻译：删除已核验句柄，不在关闭后再次按路径解析另一个文件。
    FILE_DISPOSITION_INFO disposition{TRUE};
    if(SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition, sizeof(disposition)) == FALSE) {
        return foundation::Result<bool>::failure(snapshotError("Snapshot.RemoveFailed",
            foundation::ErrorCategory::Infrastructure, "The verified snapshot could not be removed", path.value().path, GetLastError()));
    }
    return foundation::Result<bool>::success(true);
}

} // namespace lasercnc::infrastructure
