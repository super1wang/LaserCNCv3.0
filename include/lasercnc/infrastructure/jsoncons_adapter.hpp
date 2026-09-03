#pragma once

#include <lasercnc/foundation/schema.hpp>
#include <lasercnc/foundation/serialization.hpp>

namespace lasercnc::infrastructure {

class JsonconsAdapter final : public foundation::IValueSerializer, public foundation::ISchemaValidator {
public:
    [[nodiscard]] foundation::Result<std::string> serialize(
        const foundation::Value& value) const override;
    [[nodiscard]] foundation::Result<foundation::Value> deserialize(
        std::string_view payload) const override;
    [[nodiscard]] foundation::Result<void> validate(
        const foundation::Schema& schema,
        const foundation::Value& value) const override;
};

} // namespace lasercnc::infrastructure
