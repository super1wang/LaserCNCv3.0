#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
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

struct ValueBudget final {
    std::size_t maximumDepth{64U};
    std::size_t maximumNodes{100000U};
    std::size_t maximumTextBytes{16U * 1024U * 1024U};
    std::size_t maximumEncodedBytes{64U * 1024U * 1024U};
};

inline constexpr ValueBudget kernelValueBudget {};

enum class ValueBudgetViolation : std::uint8_t {
    None,
    InvalidBudget,
    Depth,
    Nodes,
    TextBytes
};

struct ValueBudgetAssessment final {
    std::size_t maximumDepth{0U};
    std::size_t nodes{0U};
    std::size_t textBytes{0U};
    ValueBudgetViolation violation{ValueBudgetViolation::None};

    [[nodiscard]] bool accepted() const noexcept
    {
        return violation == ValueBudgetViolation::None;
    }
};

// Limits passed here may tighten, but never widen, the Kernel hard ceiling.
// 中文翻译：传入限制可以收紧，但不能放宽内核硬上限。
[[nodiscard]] ValueBudgetAssessment assessValueBudget(
    const Value& value,
    const ValueBudget& budget = kernelValueBudget) noexcept;

[[nodiscard]] std::string_view valueBudgetViolationName(
    ValueBudgetViolation violation) noexcept;

} // namespace lasercnc::foundation
