#pragma once

#include <lasercnc/platform/asset_store.hpp>
#include <lasercnc/state/object_registry.hpp>

#include <span>

namespace lasercnc::runtime {

[[nodiscard]] foundation::Result<void> validateObjectAssets(
    std::span<const state::ObjectRecord> records, const platform::IAssetStore* store);

} // namespace lasercnc::runtime
