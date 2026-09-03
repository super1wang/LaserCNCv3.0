#pragma once

#include <lasercnc/kernel/identifiers.hpp>

#include <cstdint>

namespace lasercnc::state {

struct AssetRef final {
    kernel::AssetId id;
    kernel::ContentDigest digest;
    kernel::AssetKind kind;
    std::uint64_t byteSize{0U};

    friend bool operator==(const AssetRef&, const AssetRef&) = default;
};

} // namespace lasercnc::state
