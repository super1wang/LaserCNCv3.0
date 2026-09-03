#pragma once

#include <lasercnc/platform/hash_service.hpp>

namespace lasercnc::infrastructure {

class Sha256HashService final : public platform::IHashService {
public:
    [[nodiscard]] foundation::Result<kernel::ContentDigest> digest(
        std::span<const std::byte> content) const override;
};

} // namespace lasercnc::infrastructure
