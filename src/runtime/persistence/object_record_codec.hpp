#pragma once

#include <lasercnc/state/object_registry.hpp>

namespace lasercnc::persistence::detail {

[[nodiscard]] foundation::Value encodeObjectRecord(const state::ObjectRecord& record);
// Legacy containers have exactly id/type/data and map to schema 1.0.0.
// 中文翻译：旧容器仅含 id/type/data，固定映射到结构版本 1.0.0。
[[nodiscard]] foundation::Result<state::ObjectRecord> decodeObjectRecord(
    const foundation::Value& value, bool legacy);

} // namespace lasercnc::persistence::detail
