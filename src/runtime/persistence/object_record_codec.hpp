#pragma once

#include <lasercnc/state/object_registry.hpp>

namespace lasercnc::persistence::detail {

enum class ObjectRecordFormat { Legacy, Versioned, Assets };

[[nodiscard]] inline ObjectRecordFormat journalObjectFormat(std::int64_t version) noexcept
{
    return version >= 4 ? ObjectRecordFormat::Assets
                       : (version == 3 ? ObjectRecordFormat::Versioned : ObjectRecordFormat::Legacy);
}

[[nodiscard]] inline ObjectRecordFormat snapshotObjectFormat(std::int64_t version) noexcept
{
    return version >= 3 ? ObjectRecordFormat::Assets
                       : (version == 2 ? ObjectRecordFormat::Versioned : ObjectRecordFormat::Legacy);
}

[[nodiscard]] foundation::Value encodeObjectRecord(const state::ObjectRecord& record);
// Legacy containers have exactly id/type/data and map to schema 1.0.0.
// 中文翻译：旧容器仅含 id/type/data，固定映射到结构版本 1.0.0。
[[nodiscard]] foundation::Result<state::ObjectRecord> decodeObjectRecord(
    const foundation::Value& value, ObjectRecordFormat format);

} // namespace lasercnc::persistence::detail
