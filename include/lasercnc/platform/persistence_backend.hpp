#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/value.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace lasercnc::platform {

using PersistenceRow = foundation::Value::Object;

class IPersistenceBackend {
public:
    virtual ~IPersistenceBackend() = default;
    [[nodiscard]] virtual foundation::Result<std::size_t> execute(
        std::string_view statement,
        std::span<const foundation::Value> parameters = {}) = 0;
    [[nodiscard]] virtual foundation::Result<std::vector<PersistenceRow>> query(
        std::string_view statement,
        std::span<const foundation::Value> parameters = {}) = 0;
    [[nodiscard]] virtual foundation::Result<void> beginTransaction() = 0;
    [[nodiscard]] virtual foundation::Result<void> commitTransaction() = 0;
    [[nodiscard]] virtual foundation::Result<void> rollbackTransaction() = 0;
};

} // namespace lasercnc::platform
