#include <lasercnc/foundation/value.hpp>

#include <utility>

namespace lasercnc::foundation {

Value::Value() noexcept
    : storage_(nullptr)
{
}

Value::Value(std::nullptr_t) noexcept
    : storage_(nullptr)
{
}

Value::Value(bool value) noexcept
    : storage_(value)
{
}

Value::Value(std::int64_t value) noexcept
    : storage_(value)
{
}

Value::Value(double value) noexcept
    : storage_(value)
{
}

Value::Value(std::string value)
    : storage_(std::move(value))
{
}

Value::Value(const char* value)
    : storage_(nullptr)
{
    if(value != nullptr) {
        storage_ = std::string(value);
    }
}

Value::Value(Array value)
    : storage_(std::move(value))
{
}

Value::Value(Object value)
    : storage_(std::move(value))
{
}

Value::Kind Value::kind() const noexcept
{
    return static_cast<Kind>(storage_.index());
}

const Value::Storage& Value::storage() const noexcept
{
    return storage_;
}

} // namespace lasercnc::foundation
