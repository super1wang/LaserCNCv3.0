#include <lasercnc/persistence/persistence_service.hpp>

#include <lasercnc/foundation/error.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lasercnc::persistence {
namespace {

foundation::Error diagnosticPersistenceError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    foundation::Value::Object details = {},
    std::shared_ptr<const foundation::Error> cause = nullptr)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {std::move(details)},
        foundation::Severity::Error,
        std::move(cause));
}

std::span<const std::byte> bytes(std::string_view value) noexcept
{
    return {
        reinterpret_cast<const std::byte*>(value.data()),
        value.size()};
}

const char* statusName(observability::DiagnosticStatus status) noexcept
{
    switch(status) {
    case observability::DiagnosticStatus::Healthy: return "healthy";
    case observability::DiagnosticStatus::Degraded: return "degraded";
    case observability::DiagnosticStatus::Unhealthy: return "unhealthy";
    case observability::DiagnosticStatus::Unknown: return "unknown";
    }
    return "invalid";
}

foundation::Result<observability::DiagnosticStatus> parseStatus(
    std::string_view status)
{
    if(status == "healthy") {
        return foundation::Result<observability::DiagnosticStatus>::success(
            observability::DiagnosticStatus::Healthy);
    }
    if(status == "degraded") {
        return foundation::Result<observability::DiagnosticStatus>::success(
            observability::DiagnosticStatus::Degraded);
    }
    if(status == "unhealthy") {
        return foundation::Result<observability::DiagnosticStatus>::success(
            observability::DiagnosticStatus::Unhealthy);
    }
    if(status == "unknown") {
        return foundation::Result<observability::DiagnosticStatus>::success(
            observability::DiagnosticStatus::Unknown);
    }
    return foundation::Result<observability::DiagnosticStatus>::failure(
        diagnosticPersistenceError(
            "Persistence.InvalidDiagnosticStatus",
            foundation::ErrorCategory::Infrastructure,
            "Diagnostic history contains an invalid status"));
}

const foundation::Value* field(
    const foundation::Value::Object& object,
    std::string_view name) noexcept
{
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &found->second;
}

foundation::Result<std::string> textColumn(
    const platform::PersistenceRow& row,
    const char* name)
{
    const auto found = row.find(name);
    const auto* value = found == row.end()
        ? nullptr
        : found->second.getIf<std::string>();
    if(value == nullptr) {
        return foundation::Result<std::string>::failure(
            diagnosticPersistenceError(
                "Persistence.InvalidDiagnosticRow",
                foundation::ErrorCategory::Infrastructure,
                "Diagnostic history contains a missing or invalid text column",
                {{"column", foundation::Value {name}}}));
    }
    return foundation::Result<std::string>::success(*value);
}

foundation::Result<std::int64_t> integerColumn(
    const platform::PersistenceRow& row,
    const char* name)
{
    const auto found = row.find(name);
    const auto* value = found == row.end()
        ? nullptr
        : found->second.getIf<std::int64_t>();
    if(value == nullptr) {
        return foundation::Result<std::int64_t>::failure(
            diagnosticPersistenceError(
                "Persistence.InvalidDiagnosticRow",
                foundation::ErrorCategory::Infrastructure,
                "Diagnostic history contains a missing or invalid integer column",
                {{"column", foundation::Value {name}}}));
    }
    return foundation::Result<std::int64_t>::success(*value);
}

foundation::Result<std::int64_t> parseMilliseconds(std::string_view text)
{
    std::int64_t value = 0;
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if(parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size()
       || value < 0) {
        return foundation::Result<std::int64_t>::failure(
            diagnosticPersistenceError(
                "Persistence.InvalidDiagnosticPayload",
                foundation::ErrorCategory::Infrastructure,
                "Diagnostic history contains an invalid observation timestamp"));
    }
    return foundation::Result<std::int64_t>::success(value);
}

foundation::Value reportValue(
    const observability::DiagnosticReport& report,
    std::int64_t observedAtMs)
{
    return foundation::Value {foundation::Value::Object {
        {"details", report.details},
        {"diagnosticId", foundation::Value {std::string(report.id.value())}},
        {"format", foundation::Value {"lasercnc.diagnostic-report"}},
        {"observedAtMs", foundation::Value {std::to_string(observedAtMs)}},
        {"status", foundation::Value {statusName(report.status)}},
        {"summary", foundation::Value {report.summary}},
        {"version", foundation::Value {std::int64_t {1}}},
    }};
}

foundation::Result<observability::DiagnosticReport> decodeReport(
    const platform::PersistenceRow& row,
    const foundation::IValueSerializer& serializer,
    const platform::IHashService& hashes)
{
    auto sequence = integerColumn(row, "sequence");
    auto indexedIdText = textColumn(row, "diagnostic_id");
    auto indexedStatus = textColumn(row, "status");
    auto payload = textColumn(row, "payload");
    auto digestText = textColumn(row, "digest");
    auto indexedObservedAt = integerColumn(row, "observed_at_ms");
    if(!sequence || !indexedIdText || !indexedStatus || !payload || !digestText
       || !indexedObservedAt || sequence.value() <= 0
       || indexedObservedAt.value() < 0) {
        return foundation::Result<observability::DiagnosticReport>::failure(
            diagnosticPersistenceError(
                "Persistence.InvalidDiagnosticRow",
                foundation::ErrorCategory::Infrastructure,
                "A diagnostic history row is invalid"));
    }
    auto indexedId = kernel::DiagnosticId::create(indexedIdText.value());
    auto digest = kernel::ContentDigest::create(digestText.value());
    if(!indexedId || !digest) {
        return foundation::Result<observability::DiagnosticReport>::failure(
            diagnosticPersistenceError(
                "Persistence.InvalidDiagnosticRow",
                foundation::ErrorCategory::Infrastructure,
                "Diagnostic history contains an invalid stable identity"));
    }
    auto actualDigest = hashes.digest(bytes(payload.value()));
    if(!actualDigest) {
        return foundation::Result<observability::DiagnosticReport>::failure(
            std::move(actualDigest).error());
    }
    if(actualDigest.value() != digest.value()) {
        return foundation::Result<observability::DiagnosticReport>::failure(
            diagnosticPersistenceError(
                "Persistence.DiagnosticDigestMismatch",
                foundation::ErrorCategory::Infrastructure,
                "A diagnostic history payload failed its content digest check"));
    }
    auto decoded = serializer.deserialize(payload.value());
    if(!decoded) {
        return foundation::Result<observability::DiagnosticReport>::failure(
            std::move(decoded).error());
    }
    const auto* root = decoded.value().getIf<foundation::Value::Object>();
    const auto* formatValue = root == nullptr ? nullptr : field(*root, "format");
    const auto* format = formatValue == nullptr
        ? nullptr
        : formatValue->getIf<std::string>();
    const auto* versionValue = root == nullptr ? nullptr : field(*root, "version");
    const auto* version = versionValue == nullptr
        ? nullptr
        : versionValue->getIf<std::int64_t>();
    const auto* idValue = root == nullptr ? nullptr : field(*root, "diagnosticId");
    const auto* idText = idValue == nullptr
        ? nullptr
        : idValue->getIf<std::string>();
    const auto* statusValue = root == nullptr ? nullptr : field(*root, "status");
    const auto* statusText = statusValue == nullptr
        ? nullptr
        : statusValue->getIf<std::string>();
    const auto* summaryValue = root == nullptr ? nullptr : field(*root, "summary");
    const auto* summary = summaryValue == nullptr
        ? nullptr
        : summaryValue->getIf<std::string>();
    const auto* details = root == nullptr ? nullptr : field(*root, "details");
    const auto* observedValue = root == nullptr ? nullptr : field(*root, "observedAtMs");
    const auto* observedText = observedValue == nullptr
        ? nullptr
        : observedValue->getIf<std::string>();
    if(format == nullptr || *format != "lasercnc.diagnostic-report"
       || version == nullptr || *version != 1 || idText == nullptr
       || statusText == nullptr || summary == nullptr || details == nullptr
       || observedText == nullptr || *idText != indexedId.value().value()
       || *statusText != indexedStatus.value()) {
        return foundation::Result<observability::DiagnosticReport>::failure(
            diagnosticPersistenceError(
                "Persistence.InvalidDiagnosticPayload",
                foundation::ErrorCategory::Infrastructure,
                "Diagnostic payload metadata does not match its index"));
    }
    auto status = parseStatus(*statusText);
    auto observedAt = parseMilliseconds(*observedText);
    if(!status || !observedAt
       || observedAt.value() != indexedObservedAt.value()) {
        return foundation::Result<observability::DiagnosticReport>::failure(
            diagnosticPersistenceError(
                "Persistence.InvalidDiagnosticPayload",
                foundation::ErrorCategory::Infrastructure,
                "Diagnostic payload contains invalid typed metadata"));
    }
    return foundation::Result<observability::DiagnosticReport>::success(
        observability::DiagnosticReport {
            std::move(indexedId).value(),
            status.value(),
            *summary,
            *details,
            std::chrono::system_clock::time_point {
                std::chrono::milliseconds {observedAt.value()}}});
}

foundation::Result<std::vector<observability::DiagnosticReport>> decodeRows(
    const std::vector<platform::PersistenceRow>& rows,
    const foundation::IValueSerializer& serializer,
    const platform::IHashService& hashes)
{
    std::vector<observability::DiagnosticReport> reports;
    reports.reserve(rows.size());
    for(const auto& row : rows) {
        auto report = decodeReport(row, serializer, hashes);
        if(!report) {
            return foundation::Result<std::vector<observability::DiagnosticReport>>::failure(
                std::move(report).error());
        }
        reports.push_back(std::move(report).value());
    }
    return foundation::Result<std::vector<observability::DiagnosticReport>>::success(
        std::move(reports));
}

} // namespace

foundation::Result<void> PersistenceService::recordDiagnostic(
    const observability::DiagnosticReport& report)
{
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<void>::failure(diagnosticPersistenceError(
            "Persistence.NotReady",
            foundation::ErrorCategory::Conflict,
            "Persistence must be initialized before recording diagnostics"));
    }
    const auto observedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
        report.observedAt.time_since_epoch()).count();
    if(observedAt < 0 || std::string_view {statusName(report.status)} == "invalid") {
        return foundation::Result<void>::failure(diagnosticPersistenceError(
            "Persistence.InvalidDiagnosticReport",
            foundation::ErrorCategory::Validation,
            "A diagnostic report contains invalid durable metadata"));
    }
    auto payload = serializer_->serialize(reportValue(report, observedAt));
    if(!payload) {
        return foundation::Result<void>::failure(std::move(payload).error());
    }
    auto digest = hashes_->digest(bytes(payload.value()));
    if(!digest) {
        return foundation::Result<void>::failure(std::move(digest).error());
    }
    const std::array parameters {
        foundation::Value {std::string(report.id.value())},
        foundation::Value {statusName(report.status)},
        foundation::Value {payload.value()},
        foundation::Value {std::string(digest.value().value())},
        foundation::Value {observedAt}};
    auto inserted = backend_->execute(
        "INSERT INTO diagnostic_history(diagnostic_id,status,payload,digest,"
        "observed_at_ms) VALUES(?,?,?,?,?)",
        parameters);
    if(!inserted) {
        return foundation::Result<void>::failure(std::move(inserted).error());
    }
    if(inserted.value() != 1U) {
        return foundation::Result<void>::failure(diagnosticPersistenceError(
            "Persistence.DiagnosticInsertFailed",
            foundation::ErrorCategory::Infrastructure,
            "A diagnostic report was not appended exactly once"));
    }
    return foundation::Result<void>::success();
}

foundation::Result<std::vector<observability::DiagnosticReport>>
PersistenceService::diagnosticHistory(
    const kernel::DiagnosticId& diagnosticId) const
{
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<std::vector<observability::DiagnosticReport>>::failure(
            diagnosticPersistenceError(
                "Persistence.NotReady",
                foundation::ErrorCategory::Conflict,
                "Persistence must be initialized before reading diagnostics"));
    }
    const std::array parameters {
        foundation::Value {std::string(diagnosticId.value())}};
    auto rows = backend_->query(
        "SELECT sequence,diagnostic_id,status,payload,digest,observed_at_ms "
        "FROM diagnostic_history WHERE diagnostic_id=? ORDER BY sequence ASC",
        parameters);
    if(!rows) {
        return foundation::Result<std::vector<observability::DiagnosticReport>>::failure(
            std::move(rows).error());
    }
    return decodeRows(rows.value(), *serializer_, *hashes_);
}

foundation::Result<std::vector<observability::DiagnosticReport>>
PersistenceService::latestDiagnostics() const
{
    std::lock_guard lock(mutex_);
    if(!initialized_) {
        return foundation::Result<std::vector<observability::DiagnosticReport>>::failure(
            diagnosticPersistenceError(
                "Persistence.NotReady",
                foundation::ErrorCategory::Conflict,
                "Persistence must be initialized before reading diagnostics"));
    }
    auto rows = backend_->query(
        "SELECT sequence,diagnostic_id,status,payload,digest,observed_at_ms "
        "FROM diagnostic_history ORDER BY sequence ASC");
    if(!rows) {
        return foundation::Result<std::vector<observability::DiagnosticReport>>::failure(
            std::move(rows).error());
    }
    auto decoded = decodeRows(rows.value(), *serializer_, *hashes_);
    if(!decoded) {
        return decoded;
    }
    std::map<kernel::DiagnosticId, observability::DiagnosticReport> latest;
    for(auto& report : decoded.value()) {
        latest.insert_or_assign(report.id, std::move(report));
    }
    std::vector<observability::DiagnosticReport> reports;
    reports.reserve(latest.size());
    for(auto& [unused, report] : latest) {
        static_cast<void>(unused);
        reports.push_back(std::move(report));
    }
    return foundation::Result<std::vector<observability::DiagnosticReport>>::success(
        std::move(reports));
}

} // namespace lasercnc::persistence
