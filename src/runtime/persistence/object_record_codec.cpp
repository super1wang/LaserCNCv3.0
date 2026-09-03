#include "object_record_codec.hpp"

#include <lasercnc/foundation/error.hpp>

#include <array>
#include <limits>
#include <string>
#include <utility>

namespace lasercnc::persistence::detail {
namespace {

foundation::Result<state::ObjectRecord> invalidRecord()
{
    return foundation::Result<state::ObjectRecord>::failure(foundation::makeError(
        "Persistence.InvalidObjectRecord", foundation::ErrorCategory::Infrastructure,
        "A persisted object record has invalid identity, data or exact schema version"));
}

} // namespace

foundation::Value encodeObjectRecord(const state::ObjectRecord& record)
{
    return foundation::Value {foundation::Value::Object {
        {"id", foundation::Value {std::string(record.id.value())}},
        {"type", foundation::Value {std::string(record.type.value())}},
        {"data", record.data},
        {"schemaVersion", foundation::Value {foundation::Value::Object {
            {"major", foundation::Value {static_cast<std::int64_t>(record.schemaVersion.major)}},
            {"minor", foundation::Value {static_cast<std::int64_t>(record.schemaVersion.minor)}},
            {"patch", foundation::Value {static_cast<std::int64_t>(record.schemaVersion.patch)}},
        }}},
    }};
}

foundation::Result<state::ObjectRecord> decodeObjectRecord(
    const foundation::Value& value, bool legacy)
{
    const auto* root = value.getIf<foundation::Value::Object>();
    if(root == nullptr || root->size() != (legacy ? 3U : 4U)) {
        return invalidRecord();
    }
    const auto id = root->find("id");
    const auto type = root->find("type");
    const auto data = root->find("data");
    if(id == root->end() || type == root->end() || data == root->end()) {
        return invalidRecord();
    }
    const auto* idText = id->second.getIf<std::string>();
    const auto* typeText = type->second.getIf<std::string>();
    if(idText == nullptr || typeText == nullptr) {
        return invalidRecord();
    }
    auto objectId = kernel::ObjectId::create(*idText);
    auto typeId = kernel::ObjectTypeId::create(*typeText);
    if(!objectId || !typeId) {
        return invalidRecord();
    }
    foundation::Version schemaVersion{1U, 0U, 0U};
    if(!legacy) {
        const auto version = root->find("schemaVersion");
        const auto* parts = version == root->end()
            ? nullptr : version->second.getIf<foundation::Value::Object>();
        if(parts == nullptr || parts->size() != 3U) {
            return invalidRecord();
        }
        constexpr std::array names {"major", "minor", "patch"};
        std::array<std::uint32_t, 3U> values{};
        for(std::size_t index = 0U; index < names.size(); ++index) {
            const auto part = parts->find(names[index]);
            const auto* number = part == parts->end()
                ? nullptr : part->second.getIf<std::int64_t>();
            if(number == nullptr || *number < 0
               || static_cast<std::uint64_t>(*number) > std::numeric_limits<std::uint32_t>::max()) {
                return invalidRecord();
            }
            values[index] = static_cast<std::uint32_t>(*number);
        }
        schemaVersion = foundation::Version{values[0], values[1], values[2]};
    }
    return foundation::Result<state::ObjectRecord>::success(state::ObjectRecord{
        std::move(objectId).value(), std::move(typeId).value(), data->second, schemaVersion});
}

} // namespace lasercnc::persistence::detail
