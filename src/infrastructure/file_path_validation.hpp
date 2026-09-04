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

} // namespace lasercnc::infrastructure::detail
