#include <lasercnc/state/object_registry.hpp>

#include <lasercnc/foundation/error.hpp>

#include <string>
#include <utility>

namespace lasercnc::state {
namespace {

foundation::Error objectError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const kernel::ObjectId& id)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {foundation::Value::Object {
            {"objectId", foundation::Value {std::string(id.value())}},
        }});
}

} // namespace

std::size_t ObjectRegistry::size() const noexcept
{
    return objects_.size();
}

bool ObjectRegistry::empty() const noexcept
{
    return objects_.empty();
}

bool ObjectRegistry::contains(const kernel::ObjectId& id) const noexcept
{
    return objects_.contains(id);
}

const ObjectRecord* ObjectRegistry::find(const kernel::ObjectId& id) const noexcept
{
    const auto iterator = objects_.find(id);
    return iterator == objects_.end() ? nullptr : &iterator->second;
}

std::vector<ObjectRecord> ObjectRegistry::all() const
{
    std::vector<ObjectRecord> result;
    result.reserve(objects_.size());
    for(const auto& [unused, object] : objects_) {
        static_cast<void>(unused);
        result.push_back(object);
    }
    return result;
}

foundation::Result<void> ObjectRegistry::insert(ObjectRecord object)
{
    const auto id = object.id;
    const auto [unused, inserted] = objects_.emplace(id, std::move(object));
    static_cast<void>(unused);
    if(!inserted) {
        return foundation::Result<void>::failure(objectError(
            "Document.ObjectAlreadyExists",
            foundation::ErrorCategory::Conflict,
            "An object with the same stable ID already exists",
            id));
    }
    return foundation::Result<void>::success();
}

foundation::Result<void> ObjectRegistry::replaceData(
    const kernel::ObjectId& id,
    foundation::Value data)
{
    const auto iterator = objects_.find(id);
    if(iterator == objects_.end()) {
        return foundation::Result<void>::failure(objectError(
            "Document.ObjectNotFound",
            foundation::ErrorCategory::NotFound,
            "The object was not found",
            id));
    }
    iterator->second.data = std::move(data);
    return foundation::Result<void>::success();
}

foundation::Result<void> ObjectRegistry::replaceRecord(ObjectRecord object)
{
    const auto iterator = objects_.find(object.id);
    if(iterator == objects_.end()) {
        return foundation::Result<void>::failure(objectError(
            "Document.ObjectNotFound", foundation::ErrorCategory::NotFound,
            "The object was not found", object.id));
    }
    if(iterator->second.type != object.type) {
        return foundation::Result<void>::failure(objectError(
            "Document.ObjectTypeChanged", foundation::ErrorCategory::Conflict,
            "An existing object cannot change its stable type", object.id));
    }
    iterator->second = std::move(object);
    return foundation::Result<void>::success();
}

foundation::Result<void> ObjectRegistry::erase(const kernel::ObjectId& id)
{
    if(objects_.erase(id) == 0U) {
        return foundation::Result<void>::failure(objectError(
            "Document.ObjectNotFound",
            foundation::ErrorCategory::NotFound,
            "The object was not found",
            id));
    }
    return foundation::Result<void>::success();
}

void ObjectRegistry::swap(ObjectRegistry& other) noexcept
{
    objects_.swap(other.objects_);
}

} // namespace lasercnc::state
