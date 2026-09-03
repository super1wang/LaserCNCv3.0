#pragma once

#include <lasercnc/platform/asset_store.hpp>
#include <lasercnc/platform/hash_service.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>

namespace lasercnc::infrastructure {

struct FilesystemAssetStoreOptions final {
    std::filesystem::path directory;
    std::size_t maximumContentBytes{512U * 1024U * 1024U};
};

class FilesystemAssetStore final : public platform::IAssetStore {
public:
    [[nodiscard]] static foundation::Result<std::unique_ptr<FilesystemAssetStore>> create(
        FilesystemAssetStoreOptions options, std::shared_ptr<const platform::IHashService> hashes);
    ~FilesystemAssetStore() override;

    FilesystemAssetStore(const FilesystemAssetStore&) = delete;
    FilesystemAssetStore& operator=(const FilesystemAssetStore&) = delete;

    [[nodiscard]] foundation::Result<state::AssetRef> publish(
        const kernel::AssetKind& kind, std::span<const std::byte> content) override;
    [[nodiscard]] foundation::Result<std::vector<std::byte>> read(
        const state::AssetRef& reference) const override;
    [[nodiscard]] foundation::Result<void> verify(const state::AssetRef& reference) const override;

private:
    class Impl;
    explicit FilesystemAssetStore(std::unique_ptr<Impl> implementation) noexcept;
    std::unique_ptr<Impl> implementation_;
};

} // namespace lasercnc::infrastructure
