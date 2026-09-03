#pragma once

#include <lasercnc/foundation/version.hpp>
#include <lasercnc/kernel/identifiers.hpp>

#include <compare>
#include <cstdint>
#include <optional>
#include <string_view>

namespace lasercnc::runtime {

enum class VersionResolution : std::uint8_t {
    Exact,
    Compatible
};

enum class ContractStatus : std::uint8_t {
    Active,
    Deprecated
};

enum class ExecutionScope : std::uint8_t {
    Global,
    Session,
    Project,
    Document
};

struct ExecutionContext final {
    kernel::SessionId sessionId;
    std::optional<kernel::ProjectId> projectId;
    std::optional<kernel::DocumentId> documentId;

    friend bool operator==(const ExecutionContext&, const ExecutionContext&) = default;
};

[[nodiscard]] std::string_view executionScopeName(ExecutionScope scope) noexcept;
[[nodiscard]] bool validExecutionScope(ExecutionScope scope) noexcept;
[[nodiscard]] bool contextMatchesScope(
    const ExecutionContext& context,
    ExecutionScope scope) noexcept;

struct CommandKey final {
    kernel::CommandName name;
    foundation::Version version;

    friend auto operator<=>(const CommandKey&, const CommandKey&) = default;
};

struct QueryKey final {
    kernel::QueryName name;
    foundation::Version version;

    friend auto operator<=>(const QueryKey&, const QueryKey&) = default;
};

} // namespace lasercnc::runtime
