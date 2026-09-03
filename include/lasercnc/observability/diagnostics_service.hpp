#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/value.hpp>
#include <lasercnc/kernel/identifiers.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

namespace lasercnc::observability {

enum class DiagnosticStatus : std::uint8_t {
    Healthy,
    Degraded,
    Unhealthy,
    Unknown
};

struct DiagnosticReport final {
    kernel::DiagnosticId id;
    DiagnosticStatus status{DiagnosticStatus::Unknown};
    std::string summary;
    foundation::Value details;
    std::chrono::system_clock::time_point observedAt;
};

class IDiagnosticCheck {
public:
    virtual ~IDiagnosticCheck() = default;
    [[nodiscard]] virtual foundation::Result<DiagnosticReport> run() = 0;
};

class IDiagnosticExporter {
public:
    virtual ~IDiagnosticExporter() = default;
    [[nodiscard]] virtual foundation::Result<void> exportReport(
        const DiagnosticReport& report) = 0;
};

class DiagnosticsService final {
public:
    explicit DiagnosticsService(std::size_t failureCapacity = 256U);

    [[nodiscard]] foundation::Result<void> registerCheck(
        kernel::DiagnosticId id,
        std::shared_ptr<IDiagnosticCheck> check);
    [[nodiscard]] foundation::Result<void> addExporter(
        std::shared_ptr<IDiagnosticExporter> exporter);
    void freeze();
    [[nodiscard]] bool frozen() const;
    [[nodiscard]] foundation::Result<DiagnosticReport> run(
        const kernel::DiagnosticId& id);
    [[nodiscard]] std::vector<DiagnosticReport> runAll();
    [[nodiscard]] std::vector<DiagnosticReport> latest() const;
    [[nodiscard]] std::vector<foundation::Error> exporterFailures() const;

private:
    struct Entry final {
        std::shared_ptr<IDiagnosticCheck> check;
        std::optional<DiagnosticReport> latest;
    };

    mutable std::shared_mutex mutex_;
    std::map<kernel::DiagnosticId, Entry> entries_;
    std::vector<std::shared_ptr<IDiagnosticExporter>> exporters_;
    std::vector<foundation::Error> exporterFailures_;
    std::size_t failureCapacity_;
    bool frozen_{false};
};

} // namespace lasercnc::observability
