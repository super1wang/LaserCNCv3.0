#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/kernel/identifiers.hpp>

#include <cstddef>
#include <span>

namespace lasercnc::platform {

class IHashService {
public:
    virtual ~IHashService() = default;

    [[nodiscard]] virtual foundation::Result<kernel::ContentDigest> digest(
        std::span<const std::byte> content) const = 0;
};

} // namespace lasercnc::platform
