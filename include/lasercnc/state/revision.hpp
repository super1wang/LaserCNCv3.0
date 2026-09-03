#pragma once

#include <lasercnc/foundation/result.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace lasercnc::runtime {
class TransactionManager;
}

namespace lasercnc::state {

class DocumentStore;

enum class RevisionScope : std::uint8_t {
    Project,
    Document,
    Geometry,
    Cam,
    MachineContext,
    Environment
};

[[nodiscard]] std::string_view revisionScopeName(RevisionScope scope) noexcept;

class Revision final {
public:
    constexpr Revision() noexcept = default;

    explicit constexpr Revision(std::uint64_t value) noexcept
        : value_(value)
    {
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept
    {
        return value_;
    }

    [[nodiscard]] foundation::Result<Revision> next() const;

    friend auto operator<=>(const Revision&, const Revision&) = default;

private:
    std::uint64_t value_{0U};
};

struct RevisionPrecondition final {
    RevisionScope scope;
    Revision expected;
};

class RevisionSet final {
public:
    RevisionSet() = default;
    RevisionSet(
        Revision project,
        Revision document,
        Revision geometry,
        Revision cam,
        Revision machineContext,
        Revision environment) noexcept;

    [[nodiscard]] const Revision& at(RevisionScope scope) const noexcept;

    friend bool operator==(const RevisionSet&, const RevisionSet&) = default;

private:
    friend class DocumentStore;
    friend class RevisionManager;
    friend class runtime::TransactionManager;

    static constexpr std::size_t scopeCount = 6U;

    [[nodiscard]] Revision& atMutable(RevisionScope scope) noexcept;

    std::array<Revision, scopeCount> values_ {};
};

class RevisionManager final {
public:
    [[nodiscard]] static foundation::Result<void> validate(
        const RevisionSet& current,
        std::span<const RevisionPrecondition> preconditions);
    [[nodiscard]] static foundation::Result<RevisionSet> advance(
        const RevisionSet& current,
        std::span<const RevisionScope> scopes);
};

} // namespace lasercnc::state
