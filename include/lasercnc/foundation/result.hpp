#pragma once

#include <lasercnc/foundation/error.hpp>

#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace lasercnc::foundation {

template <typename T>
class [[nodiscard]] Result final {
public:
    static_assert(!std::is_reference_v<T>, "Result<T> 不支持引用类型");
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Error>, "Result<Error> 会产生歧义");

    [[nodiscard]] static Result success(T value)
    {
        return Result(std::move(value));
    }

    [[nodiscard]] static Result failure(Error error)
    {
        return Result(std::move(error));
    }

    [[nodiscard]] bool hasValue() const noexcept
    {
        return std::holds_alternative<T>(storage_);
    }

    explicit operator bool() const noexcept
    {
        return hasValue();
    }

    [[nodiscard]] T& value() &
    {
        ensureValue();
        return std::get<T>(storage_);
    }

    [[nodiscard]] const T& value() const&
    {
        ensureValue();
        return std::get<T>(storage_);
    }

    [[nodiscard]] T&& value() &&
    {
        ensureValue();
        return std::get<T>(std::move(storage_));
    }

    [[nodiscard]] const Error& error() const&
    {
        ensureError();
        return std::get<Error>(storage_);
    }

    [[nodiscard]] Error&& error() &&
    {
        ensureError();
        return std::get<Error>(std::move(storage_));
    }

private:
    explicit Result(T value)
        : storage_(std::move(value))
    {
    }

    explicit Result(Error error)
        : storage_(std::move(error))
    {
    }

    void ensureValue() const
    {
        if(!hasValue()) {
            throw std::logic_error("Result does not contain a value");
        }
    }

    void ensureError() const
    {
        if(hasValue()) {
            throw std::logic_error("Result does not contain an error");
        }
    }

    std::variant<T, Error> storage_;
};

template <>
class [[nodiscard]] Result<void> final {
public:
    [[nodiscard]] static Result success()
    {
        return Result();
    }

    [[nodiscard]] static Result failure(Error error)
    {
        return Result(std::move(error));
    }

    [[nodiscard]] bool hasValue() const noexcept
    {
        return !error_.has_value();
    }

    explicit operator bool() const noexcept
    {
        return hasValue();
    }

    [[nodiscard]] const Error& error() const&
    {
        if(hasValue()) {
            throw std::logic_error("Result does not contain an error");
        }
        return *error_;
    }

    [[nodiscard]] Error&& error() &&
    {
        if(hasValue()) {
            throw std::logic_error("Result does not contain an error");
        }
        return std::move(*error_);
    }

private:
    Result() = default;

    explicit Result(Error error)
        : error_(std::move(error))
    {
    }

    std::optional<Error> error_;
};

} // namespace lasercnc::foundation
