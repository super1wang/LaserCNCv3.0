#include <lasercnc/infrastructure/filesystem_snapshot_store.hpp>

#include <lasercnc/foundation/error.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace lasercnc::infrastructure {
namespace {

std::atomic_ullong temporarySequence{0U};

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

bool safeSnapshotId(std::string_view value) noexcept
{
    if(value.empty() || value == "." || value == ".." || value.size() > 128U) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '.' || character == '_'
            || character == '-';
    });
}

class FileHandle final {
public:
    explicit FileHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}
    ~FileHandle() { reset(); }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

    void reset() noexcept
    {
        if(valid()) {
            static_cast<void>(CloseHandle(handle_));
            handle_ = INVALID_HANDLE_VALUE;
        }
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
    std::size_t maximumPayloadBytes)
{
    FileHandle file(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if(!file.valid()) {
        const auto error = GetLastError();
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
    LARGE_INTEGER size {};
    if(GetFileSizeEx(file.get(), &size) == FALSE || size.QuadPart < 0
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
               file.get(),
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

} // namespace

class FilesystemSnapshotStore::Impl final {
public:
    Impl(std::filesystem::path directory, std::size_t maximumPayloadBytes)
        : directory_(std::move(directory)), maximumPayloadBytes_(maximumPayloadBytes)
    {
    }

    foundation::Result<std::filesystem::path> pathFor(
        const kernel::SnapshotId& snapshotId) const
    {
        if(!safeSnapshotId(snapshotId.value())) {
            return foundation::Result<std::filesystem::path>::failure(snapshotError(
                "Snapshot.InvalidIdForStore",
                foundation::ErrorCategory::Validation,
                "The snapshot identity cannot be mapped to a safe file name",
                directory_));
        }
        return foundation::Result<std::filesystem::path>::success(
            directory_ / (std::string(snapshotId.value()) + ".snapshot"));
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
    if(options.directory.empty() || options.maximumPayloadBytes == 0U) {
        return foundation::Result<std::unique_ptr<FilesystemSnapshotStore>>::failure(
            snapshotError(
                "Snapshot.InvalidStoreOptions",
                foundation::ErrorCategory::Validation,
                "The snapshot store requires a directory and a positive size limit",
                options.directory));
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
    auto target = implementation_->pathFor(snapshotId);
    if(!target) {
        return foundation::Result<platform::SnapshotWriteDisposition>::failure(
            std::move(target).error());
    }
    if(payload.size() > implementation_->maximumPayloadBytes_) {
        return foundation::Result<platform::SnapshotWriteDisposition>::failure(snapshotError(
            "Snapshot.PayloadTooLarge",
            foundation::ErrorCategory::Validation,
            "The snapshot payload exceeds the configured size limit",
            target.value()));
    }
    std::error_code existsError;
    const bool targetExists = std::filesystem::exists(target.value(), existsError);
    if(existsError) {
        return foundation::Result<platform::SnapshotWriteDisposition>::failure(snapshotError(
            "Snapshot.ReadFailed",
            foundation::ErrorCategory::Infrastructure,
            "The snapshot destination could not be inspected",
            target.value(),
            static_cast<std::uint32_t>(existsError.value())));
    }
    if(targetExists) {
        auto existing = readFile(target.value(), implementation_->maximumPayloadBytes_);
        if(!existing) {
            return foundation::Result<platform::SnapshotWriteDisposition>::failure(
                std::move(existing).error());
        }
        if(existing.value() != payload) {
            return foundation::Result<platform::SnapshotWriteDisposition>::failure(snapshotError(
                "Snapshot.IdentityConflict",
                foundation::ErrorCategory::Conflict,
                "A snapshot identity is already bound to different content",
                target.value()));
        }
        return foundation::Result<platform::SnapshotWriteDisposition>::success(
            platform::SnapshotWriteDisposition::AlreadyPresent);
    }

    const auto temporary = target.value().wstring() + L".tmp."
        + std::to_wstring(GetCurrentProcessId()) + L'.' + std::to_wstring(GetTickCount64()) + L'.'
        + std::to_wstring(temporarySequence.fetch_add(1U));
    FileHandle file(CreateFileW(
        temporary.c_str(),
        GENERIC_WRITE,
        0U,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr));
    if(!file.valid()) {
        return foundation::Result<platform::SnapshotWriteDisposition>::failure(snapshotError(
            "Snapshot.WriteFailed",
            foundation::ErrorCategory::Infrastructure,
            "The temporary snapshot file could not be created",
            temporary,
            GetLastError()));
    }
    auto written = writeAll(file.get(), payload);
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
           temporary.c_str(), target.value().c_str(), MOVEFILE_WRITE_THROUGH)
       == FALSE) {
        const auto error = GetLastError();
        static_cast<void>(DeleteFileW(temporary.c_str()));
        if(error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
            auto existing = readFile(target.value(), implementation_->maximumPayloadBytes_);
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
                        target.value()));
            }
            return foundation::Result<platform::SnapshotWriteDisposition>::failure(
                std::move(existing).error());
        }
        return foundation::Result<platform::SnapshotWriteDisposition>::failure(snapshotError(
            "Snapshot.PublishFailed",
            foundation::ErrorCategory::Infrastructure,
            "The temporary snapshot file could not be published atomically",
            target.value(),
            error));
    }
    return foundation::Result<platform::SnapshotWriteDisposition>::success(
        platform::SnapshotWriteDisposition::Created);
}

foundation::Result<std::string> FilesystemSnapshotStore::read(
    const kernel::SnapshotId& snapshotId) const
{
    std::lock_guard lock(implementation_->mutex_);
    auto path = implementation_->pathFor(snapshotId);
    if(!path) {
        return foundation::Result<std::string>::failure(std::move(path).error());
    }
    return readFile(path.value(), implementation_->maximumPayloadBytes_);
}

foundation::Result<bool> FilesystemSnapshotStore::remove(
    const kernel::SnapshotId& snapshotId)
{
    std::lock_guard lock(implementation_->mutex_);
    auto path = implementation_->pathFor(snapshotId);
    if(!path) {
        return foundation::Result<bool>::failure(std::move(path).error());
    }
    if(DeleteFileW(path.value().c_str()) != FALSE) {
        return foundation::Result<bool>::success(true);
    }
    const auto error = GetLastError();
    if(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return foundation::Result<bool>::success(false);
    }
    return foundation::Result<bool>::failure(snapshotError(
        "Snapshot.RemoveFailed",
        foundation::ErrorCategory::Infrastructure,
        "The snapshot payload could not be removed",
        path.value(),
        error));
}

} // namespace lasercnc::infrastructure
