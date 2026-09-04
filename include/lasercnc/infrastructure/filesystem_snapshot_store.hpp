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
    // Logical IDs are exact bytes (up to 4096), never paths. New files use hashed names
    // and an identity-bound envelope; payload budgets exclude that envelope. Legacy
    // ASCII files require exact filename case; ambiguous formats and links are rejected.
    // 中文翻译：逻辑 ID 按精确字节比较（最多 4096），不是路径；新文件使用摘要名和身份信封，
    // 负载预算不含信封。旧 ASCII 文件必须大小写精确匹配；拒绝格式歧义与链接。
    // Directory components ending in dots/spaces are rejected before using native long paths.
    // 中文翻译：进入原生长路径前拒绝末尾带点或空格的目录组件，避免改变旧路径含义。
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
