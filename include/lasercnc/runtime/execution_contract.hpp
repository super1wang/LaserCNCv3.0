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

enum class ReplayPolicy : std::uint8_t {
    Safe,
    Idempotent,
    ReconcileOnly,
    Never
};

enum class RecoveryDisposition : std::uint8_t {
    Completed,
    Interrupted,
    Indeterminate,
    ReconcileRequired
};

enum class ExternalEffectState : std::uint8_t {
    Executing,
    Completed,
    Interrupted,
    Indeterminate,
    ReconcileRequired
};

struct ExecutionContext final {
    kernel::SessionId sessionId;
    std::optional<kernel::ProjectId> projectId;
    std::optional<kernel::DocumentId> documentId;

    friend bool operator==(const ExecutionContext&, const ExecutionContext&) = default;
};

[[nodiscard]] std::string_view executionScopeName(ExecutionScope scope) noexcept;
[[nodiscard]] bool validExecutionScope(ExecutionScope scope) noexcept;
[[nodiscard]] bool validContractStatus(ContractStatus status) noexcept;
[[nodiscard]] bool contextMatchesScope(
    const ExecutionContext& context,
    ExecutionScope scope) noexcept;
[[nodiscard]] std::string_view replayPolicyName(ReplayPolicy policy) noexcept;
[[nodiscard]] bool validReplayPolicy(ReplayPolicy policy) noexcept;
[[nodiscard]] std::string_view recoveryDispositionName(
    RecoveryDisposition disposition) noexcept;
[[nodiscard]] std::string_view externalEffectStateName(
    ExternalEffectState state) noexcept;
[[nodiscard]] bool validExternalEffectState(ExternalEffectState state) noexcept;
[[nodiscard]] RecoveryDisposition interruptedDisposition(
    ReplayPolicy policy) noexcept;
[[nodiscard]] bool explicitRetryAllowed(ReplayPolicy policy) noexcept;

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
