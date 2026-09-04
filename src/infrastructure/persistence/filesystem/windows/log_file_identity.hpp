#pragma once
#include <array>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace lasercnc::infrastructure::detail {
struct FileIdentity final {
    std::uint64_t volume;
    std::array<unsigned char, 16> identifier;
    auto operator<=>(const FileIdentity&) const = default;
};
struct TargetName final {
    FileIdentity directory;
    std::wstring relative;
    bool caseSensitive;
};
struct NameLess final {
    bool operator()(const TargetName& left, const TargetName& right) const;
};
struct InspectedLogFile final {
    TargetName name;
    std::optional<FileIdentity> existing;
};
// Inspect only: no file creation, renaming or retained ownership is performed.
// 中文翻译：只检查元数据，不创建、重命名文件，也不持有长期独占权。
[[nodiscard]] InspectedLogFile inspectLogFileTarget(const std::filesystem::path& path);
} // namespace lasercnc::infrastructure::detail
