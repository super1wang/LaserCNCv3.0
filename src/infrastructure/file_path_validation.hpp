#pragma once

#include <filesystem>

namespace lasercnc::infrastructure::detail {

// Validate native code units before any conversion or null-terminated OS/library call.
// 中文翻译：在任何编码转换或以空字符结束的系统/库调用之前，先验证原生路径码元。
[[nodiscard]] inline bool containsEmbeddedNull(const std::filesystem::path& path) noexcept
{
    return path.native().find(std::filesystem::path::value_type{})
        != std::filesystem::path::string_type::npos;
}

// Reject unpaired UTF-16 surrogates before conversion or diagnostic formatting.
// 中文翻译：在编码转换或诊断格式化之前拒绝未配对的 UTF-16 代理码元。
[[nodiscard]] inline bool containsMalformedUtf16(const std::filesystem::path& path) noexcept
{
    const auto& units = path.native();
    for(std::size_t index = 0U; index < units.size(); ++index) {
        const auto unit = units[index];
        if(unit >= 0xd800 && unit <= 0xdbff) {
            if(index + 1U == units.size() || units[index + 1U] < 0xdc00 || units[index + 1U] > 0xdfff) {
                return true;
            }
            ++index;
        } else if(unit >= 0xdc00 && unit <= 0xdfff) {
            return true;
        }
    }
    return false;
}

} // namespace lasercnc::infrastructure::detail
