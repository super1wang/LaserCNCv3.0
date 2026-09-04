#include <lasercnc/observability/diagnostics_service.hpp>

#include <lasercnc/foundation/error.hpp>

#include <exception>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace lasercnc::observability {
namespace {

foundation::Error diagnosticError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const kernel::DiagnosticId& id)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {foundation::Value::Object {
            {"diagnostic", foundation::Value {std::string(id.value())}},
        }});
}

DiagnosticReport unhealthy(
    const kernel::DiagnosticId& id,
    std::string summary,
    foundation::Value details)
{
    return DiagnosticReport {
        id,
        DiagnosticStatus::Unhealthy,
        std::move(summary),
        std::move(details),
        std::chrono::system_clock::now()};
}

class SerializedDiagnosticCheck final : public IDiagnosticCheck {
public:
    class Execution final {
    public:
        explicit Execution(SerializedDiagnosticCheck& owner) : owner_(&owner) {}
        Execution(const Execution&) = delete;
        Execution& operator=(const Execution&) = delete;
        Execution(Execution&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
        Execution& operator=(Execution&&) = delete;
        ~Execution()
        {
            if(owner_ != nullptr) {
                try { owner_->release(); }
                catch(...) {}
            }
        }
    private:
        SerializedDiagnosticCheck* owner_;
    };

    explicit SerializedDiagnosticCheck(std::shared_ptr<IDiagnosticCheck> check)
        : check_(std::move(check))
    {
    }

    std::optional<Execution> acquire()
    {
        std::unique_lock lock(mutex_);
        const auto caller = std::this_thread::get_id();
        if(running_ && owner_ == caller) {
            return std::nullopt;
        }
        available_.wait(lock, [this] { return !running_; });
        running_ = true;
        owner_ = caller;
        return Execution{*this};
    }

    foundation::Result<DiagnosticReport> run() override
    {
        return check_->run();
    }

private:
    void release()
    {
        {
            std::lock_guard lock(mutex_);
            running_ = false;
            owner_ = {};
        }
        available_.notify_one();
    }

    std::shared_ptr<IDiagnosticCheck> check_;
    std::mutex mutex_;
    std::condition_variable available_;
    std::thread::id owner_;
    bool running_{false};
};

} // namespace

DiagnosticsService::DiagnosticsService(std::size_t failureCapacity)
    : failureCapacity_(std::max<std::size_t>(failureCapacity, 1U))
{
}

foundation::Result<void> DiagnosticsService::registerCheck(
    kernel::DiagnosticId id,
    std::shared_ptr<IDiagnosticCheck> check)
{
    if(check == nullptr) {
        return foundation::Result<void>::failure(diagnosticError(
            "Diagnostics.InvalidCheck",
            foundation::ErrorCategory::Validation,
            "A diagnostic check is required",
            id));
    }
    // One registered entry executes through local latest publication at a time. Allocate the
    // wrapper before taking the registry lock so rejection cannot release user ownership there.
    // 中文翻译：同一注册项串行执行至本地 latest 发布；包装器在注册表锁外分配。
    auto serialized = std::make_shared<SerializedDiagnosticCheck>(check);
    std::unique_lock lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::failure(diagnosticError(
            "Diagnostics.RegistryFrozen",
            foundation::ErrorCategory::Conflict,
            "Diagnostic registration is frozen",
            id));
    }
    const auto [unused, inserted] = entries_.emplace(
        // Keep the parameter owner until after the lock leaves scope, including rejection.
        // 中文翻译：保留参数所有者直到锁离开作用域，拒绝时也不在锁内销毁最后一个检查对象。
        id, Entry {std::move(serialized), std::nullopt});
    static_cast<void>(unused);
    if(!inserted) {
        return foundation::Result<void>::failure(diagnosticError(
            "Diagnostics.AlreadyRegistered",
            foundation::ErrorCategory::Conflict,
            "A diagnostic check with the same stable id already exists",
            id));
    }
    return foundation::Result<void>::success();
}

foundation::Result<void> DiagnosticsService::addExporter(
    std::shared_ptr<IDiagnosticExporter> exporter)
{
    if(exporter == nullptr) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Diagnostics.InvalidExporter",
            foundation::ErrorCategory::Validation,
            "A diagnostic exporter is required"));
    }
    std::unique_lock lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::failure(foundation::makeError(
            "Diagnostics.ExportersFrozen",
            foundation::ErrorCategory::Conflict,
            "Diagnostic exporters are frozen"));
    }
    exporters_.push_back(std::move(exporter));
    return foundation::Result<void>::success();
}

void DiagnosticsService::freeze()
{
    std::unique_lock lock(mutex_);
    frozen_ = true;
}

bool DiagnosticsService::frozen() const
{
    std::shared_lock lock(mutex_);
    return frozen_;
}

foundation::Result<DiagnosticReport> DiagnosticsService::run(
    const kernel::DiagnosticId& id)
{
    std::shared_ptr<IDiagnosticCheck> check;
    std::shared_ptr<SerializedDiagnosticCheck> serialized;
    {
        std::shared_lock lock(mutex_);
        const auto found = entries_.find(id);
        if(found == entries_.end()) {
            return foundation::Result<DiagnosticReport>::failure(diagnosticError(
                "Diagnostics.NotFound",
                foundation::ErrorCategory::NotFound,
                "The diagnostic check is not registered",
                id));
        }
        check = found->second.check;
        serialized = std::static_pointer_cast<SerializedDiagnosticCheck>(check);
    }

    auto execution = serialized->acquire();

    DiagnosticReport report = [&]() {
        if(!execution.has_value()) {
            return unhealthy(id, "Diagnostic check recursively invoked its own registered entry",
                foundation::Value{foundation::Value::Object{
                    {"errorCode", foundation::Value{"Diagnostics.CheckReentered"}}}});
        }
        try {
            auto checked = check->run();
            if(!checked) {
                const auto error = std::move(checked).error();
                return unhealthy(
                    id,
                    "Diagnostic check returned an error",
                    foundation::Value {foundation::Value::Object {
                        {"errorCode", foundation::Value {std::string(error.code.value())}},
                        {"message", foundation::Value {error.message}},
                    }});
            }
            auto value = std::move(checked).value();
            if(value.id != id) {
                return unhealthy(
                    id,
                    "Diagnostic check returned a mismatched id",
                    foundation::Value {foundation::Value::Object {
                        {"reportedId", foundation::Value {std::string(value.id.value())}},
                    }});
            }
            switch(value.status) {
            case DiagnosticStatus::Healthy:
            case DiagnosticStatus::Degraded:
            case DiagnosticStatus::Unhealthy:
            case DiagnosticStatus::Unknown:
                break;
            default:
                return unhealthy(id, "Diagnostic check returned an invalid status",
                    foundation::Value{foundation::Value::Object{
                        {"errorCode", foundation::Value{"Diagnostics.InvalidStatus"}},
                        {"reportedStatus", foundation::Value{static_cast<std::int64_t>(value.status)}}}});
            }
            value.observedAt = std::chrono::system_clock::now();
            return value;
        } catch(const std::exception& exception) {
            return unhealthy(
                id,
                "Diagnostic check raised an exception",
                foundation::Value {foundation::Value::Object {
                    {"reason", foundation::Value {exception.what()}},
                }});
        } catch(...) {
            return unhealthy(
                id,
                "Diagnostic check raised an unknown exception",
                foundation::Value {});
        }
    }();

    std::vector<std::shared_ptr<IDiagnosticExporter>> exporters;
    {
        std::unique_lock lock(mutex_);
        const auto found = entries_.find(id);
        if(found != entries_.end()) {
            found->second.latest = report;
        }
        exporters = exporters_;
    }
    // The same registered entry may run again only after its report and exporter snapshot are
    // locally published. Export calls themselves remain outside both service and entry locks.
    // 中文翻译：同一注册项须等待报告与出口快照完成本地发布；出口调用仍在两类锁外执行。
    execution.reset();

    const auto retainFailure = [this](foundation::Error failure) noexcept {
        try {
            std::unique_lock lock(mutex_);
            if(exporterFailures_.size() >= failureCapacity_) {
                exporterFailures_.erase(exporterFailures_.begin());
            }
            exporterFailures_.push_back(std::move(failure));
        } catch(...) {
        }
    };
    for(const auto& exporter : exporters) {
        try {
            auto exported = exporter->exportReport(report);
            if(!exported) {
                retainFailure(std::move(exported).error());
            }
        } catch(const std::exception& exception) {
            try {
                retainFailure(diagnosticError(
                    "Diagnostics.ExporterThrew",
                    foundation::ErrorCategory::Internal,
                    exception.what(),
                    id));
            } catch(...) {
            }
        } catch(...) {
            try {
                retainFailure(diagnosticError(
                    "Diagnostics.ExporterThrew",
                    foundation::ErrorCategory::Internal,
                    "A diagnostic exporter raised an unknown exception",
                    id));
            } catch(...) {
            }
        }
    }
    return foundation::Result<DiagnosticReport>::success(std::move(report));
}

std::vector<DiagnosticReport> DiagnosticsService::runAll()
{
    std::vector<kernel::DiagnosticId> ids;
    {
        std::shared_lock lock(mutex_);
        ids.reserve(entries_.size());
        for(const auto& [id, unused] : entries_) {
            static_cast<void>(unused);
            ids.push_back(id);
        }
    }
    std::vector<DiagnosticReport> reports;
    reports.reserve(ids.size());
    for(const auto& id : ids) {
        auto report = run(id);
        if(report) {
            reports.push_back(std::move(report).value());
        }
    }
    return reports;
}

std::vector<DiagnosticReport> DiagnosticsService::latest() const
{
    std::shared_lock lock(mutex_);
    std::vector<DiagnosticReport> reports;
    reports.reserve(entries_.size());
    for(const auto& [unused, entry] : entries_) {
        static_cast<void>(unused);
        if(entry.latest.has_value()) {
            reports.push_back(*entry.latest);
        }
    }
    return reports;
}

std::vector<foundation::Error> DiagnosticsService::exporterFailures() const
{
    std::shared_lock lock(mutex_);
    return exporterFailures_;
}

} // namespace lasercnc::observability
