#pragma once

#include <lasercnc/platform/snapshot_store.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>

namespace lasercnc::infrastructure {

struct FilesystemSnapshotStoreOptions final {
    std::filesystem::path directory;
    std::size_t maximumPayloadBytes{64U * 1024U * 1024U};
};

class FilesystemSnapshotStore final : public platform::ISnapshotStore {
public:
    [[nodiscard]] static foundation::Result<std::unique_ptr<FilesystemSnapshotStore>> create(
        FilesystemSnapshotStoreOptions options);

    ~FilesystemSnapshotStore() override;

    FilesystemSnapshotStore(const FilesystemSnapshotStore&) = delete;
    FilesystemSnapshotStore& operator=(const FilesystemSnapshotStore&) = delete;

    [[nodiscard]] foundation::Result<platform::SnapshotWriteDisposition> writeAtomically(
        const kernel::SnapshotId& snapshotId,
        std::string_view payload) override;
    [[nodiscard]] foundation::Result<std::string> read(
        const kernel::SnapshotId& snapshotId) const override;
    [[nodiscard]] foundation::Result<bool> remove(
        const kernel::SnapshotId& snapshotId) override;

private:
    class Impl;
    explicit FilesystemSnapshotStore(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace lasercnc::infrastructure
