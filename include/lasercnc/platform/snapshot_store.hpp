#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/kernel/identifiers.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace lasercnc::platform {

enum class SnapshotWriteDisposition : std::uint8_t {
    Created,
    AlreadyPresent
};

class ISnapshotStore {
public:
    virtual ~ISnapshotStore() = default;

    [[nodiscard]] virtual foundation::Result<SnapshotWriteDisposition> writeAtomically(
        const kernel::SnapshotId& snapshotId,
        std::string_view payload) = 0;
    [[nodiscard]] virtual foundation::Result<std::string> read(
        const kernel::SnapshotId& snapshotId) const = 0;
    [[nodiscard]] virtual foundation::Result<bool> remove(
        const kernel::SnapshotId& snapshotId) = 0;
};

} // namespace lasercnc::platform
