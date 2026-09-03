#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace lasercnc::foundation {

class Value final {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;
    using Storage = std::variant<
        std::nullptr_t,
        bool,
        std::int64_t,
        double,
        std::string,
        Array,
        Object>;

    enum class Kind : std::uint8_t {
        Null,
        Boolean,
        Integer,
        Number,
        String,
        Array,
        Object
    };

    Value() noexcept;
    Value(std::nullptr_t) noexcept;
    explicit Value(bool value) noexcept;
    explicit Value(std::int64_t value) noexcept;
    explicit Value(double value) noexcept;
    explicit Value(std::string value);
    explicit Value(const char* value);
    explicit Value(Array value);
    explicit Value(Object value);

    [[nodiscard]] Kind kind() const noexcept;
    [[nodiscard]] const Storage& storage() const noexcept;

    template <typename T>
    [[nodiscard]] const T* getIf() const noexcept
    {
        return std::get_if<T>(&storage_);
    }

    template <typename T>
    [[nodiscard]] T* getIf() noexcept
    {
        return std::get_if<T>(&storage_);
    }

    friend bool operator==(const Value&, const Value&) = default;

private:
    Storage storage_;
};

} // namespace lasercnc::foundation
