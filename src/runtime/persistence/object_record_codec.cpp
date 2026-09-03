#include "object_record_codec.hpp"

#include <lasercnc/foundation/error.hpp>

#include <array>
#include <charconv>
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
    foundation::Value::Array assets;
    for(const auto& reference : record.assets) {
        assets.emplace_back(foundation::Value::Object{
            {"id", foundation::Value{std::string(reference.id.value())}},
            {"digest", foundation::Value{std::string(reference.digest.value())}},
            {"kind", foundation::Value{std::string(reference.kind.value())}},
            {"byteSize", foundation::Value{std::to_string(reference.byteSize)}},
        });
    }
    return foundation::Value {foundation::Value::Object {
        {"assets", foundation::Value{std::move(assets)}},
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
    const foundation::Value& value, ObjectRecordFormat format)
{
    const bool legacy = format == ObjectRecordFormat::Legacy;
    const bool hasAssets = format == ObjectRecordFormat::Assets;
    const auto* root = value.getIf<foundation::Value::Object>();
    if(root == nullptr || root->size() != (legacy ? 3U : (hasAssets ? 5U : 4U))) {
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
    std::vector<state::AssetRef> assets;
    if(hasAssets) {
        const auto entry = root->find("assets");
        const auto* references = entry == root->end() ? nullptr : entry->second.getIf<foundation::Value::Array>();
        if(references == nullptr) {
            return invalidRecord();
        }
        for(const auto& reference : *references) {
            const auto* fields = reference.getIf<foundation::Value::Object>();
            if(fields == nullptr || fields->size() != 4U) {
                return invalidRecord();
            }
            const auto text = [&](const char* name) -> const std::string* {
                const auto field = fields->find(name);
                return field == fields->end() ? nullptr : field->second.getIf<std::string>();
            };
            const auto* assetId = text("id");
            const auto* digest = text("digest");
            const auto* kind = text("kind");
            const auto* size = text("byteSize");
            if(assetId == nullptr || digest == nullptr || kind == nullptr || size == nullptr) {
                return invalidRecord();
            }
            auto parsedId = kernel::AssetId::create(*assetId);
            auto parsedDigest = kernel::ContentDigest::create(*digest);
            auto parsedKind = kernel::AssetKind::create(*kind);
            std::uint64_t byteSize = 0U;
            const auto parsedSize = std::from_chars(size->data(), size->data() + size->size(), byteSize);
            if(!parsedId || !parsedDigest || !parsedKind || parsedSize.ec != std::errc{}
               || parsedSize.ptr != size->data() + size->size() || std::to_string(byteSize) != *size) {
                return invalidRecord();
            }
            assets.push_back({std::move(parsedId).value(), std::move(parsedDigest).value(),
                std::move(parsedKind).value(), byteSize});
        }
    }
    return foundation::Result<state::ObjectRecord>::success(state::ObjectRecord{
        std::move(objectId).value(), std::move(typeId).value(), data->second, schemaVersion, std::move(assets)});
}

} // namespace lasercnc::persistence::detail
