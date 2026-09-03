#pragma once

#include <lasercnc/platform/config_serializer.hpp>

namespace lasercnc::infrastructure {

class TomlConfigAdapter final : public platform::IConfigSerializer {
public:
    [[nodiscard]] foundation::Result<foundation::Value> parse(
        std::string_view content,
        std::string_view sourceName) const override;
    [[nodiscard]] foundation::Result<std::string> serialize(
        const foundation::Value& root) const override;
};

} // namespace lasercnc::infrastructure
