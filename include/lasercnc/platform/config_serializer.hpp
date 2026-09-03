#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/value.hpp>

#include <string>
#include <string_view>

namespace lasercnc::platform {

class IConfigSerializer {
public:
    virtual ~IConfigSerializer() = default;
    [[nodiscard]] virtual foundation::Result<foundation::Value> parse(
        std::string_view content,
        std::string_view sourceName) const = 0;
    [[nodiscard]] virtual foundation::Result<std::string> serialize(
        const foundation::Value& root) const = 0;
};

} // namespace lasercnc::platform
