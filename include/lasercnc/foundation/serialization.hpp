#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/value.hpp>

#include <string>
#include <string_view>

namespace lasercnc::foundation {

class IValueSerializer {
public:
    virtual ~IValueSerializer() = default;

    [[nodiscard]] virtual Result<std::string> serialize(const Value& value) const = 0;
    [[nodiscard]] virtual Result<Value> deserialize(std::string_view payload) const = 0;
};

} // namespace lasercnc::foundation
