#pragma once

#include <lasercnc/foundation/version.hpp>
#include <lasercnc/kernel/identifiers.hpp>

#include <compare>
#include <cstdint>

namespace lasercnc::runtime {

enum class VersionResolution : std::uint8_t {
    Exact,
    Compatible
};

enum class ContractStatus : std::uint8_t {
    Active,
    Deprecated
};

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
