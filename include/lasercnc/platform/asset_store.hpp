#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/state/asset_ref.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace lasercnc::platform {

class IAssetStore {
public:
    virtual ~IAssetStore() = default;
    [[nodiscard]] virtual foundation::Result<state::AssetRef> publish(
        const kernel::AssetKind& kind, std::span<const std::byte> content) = 0;
    [[nodiscard]] virtual foundation::Result<std::vector<std::byte>> read(
        const state::AssetRef& reference) const = 0;
    [[nodiscard]] virtual foundation::Result<void> verify(
        const state::AssetRef& reference) const = 0;
};

} // namespace lasercnc::platform
