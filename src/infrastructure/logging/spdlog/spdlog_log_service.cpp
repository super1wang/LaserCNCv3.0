#include <lasercnc/infrastructure/spdlog_log_service.hpp>
#include "../../file_path_validation.hpp"

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/infrastructure/jsoncons_adapter.hpp>

#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lasercnc::infrastructure {
namespace {

const char* levelName(observability::LogLevel level) noexcept
{
    switch(level) {
    case observability::LogLevel::Trace: return "trace";
    case observability::LogLevel::Debug: return "debug";
    case observability::LogLevel::Info: return "info";
    case observability::LogLevel::Warning: return "warning";
    case observability::LogLevel::Error: return "error";
    case observability::LogLevel::Critical: return "critical";
    }
    return "unknown";
}

spdlog::level::level_enum toSpdlogLevel(observability::LogLevel level) noexcept
{
    switch(level) {
    case observability::LogLevel::Trace: return spdlog::level::trace;
    case observability::LogLevel::Debug: return spdlog::level::debug;
    case observability::LogLevel::Info: return spdlog::level::info;
    case observability::LogLevel::Warning: return spdlog::level::warn;
    case observability::LogLevel::Error: return spdlog::level::err;
    case observability::LogLevel::Critical: return spdlog::level::critical;
    }
    return spdlog::level::off;
}

std::string formatTimestamp(std::chrono::system_clock::time_point timestamp)
{
    using namespace std::chrono;
    const auto secondsSinceEpoch = floor<seconds>(timestamp);
    const auto millisecondPart = duration_cast<milliseconds>(timestamp - secondsSinceEpoch);

    const auto time = system_clock::to_time_t(secondsSinceEpoch);
    std::tm utc {};
#if defined(_WIN32)
    if(gmtime_s(&utc, &time) != 0) {
        throw std::runtime_error("Cannot convert log timestamp to UTC");
    }
#else
    if(gmtime_r(&time, &utc) == nullptr) {
        throw std::runtime_error("Cannot convert log timestamp to UTC");
    }
#endif

    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3)
           << std::setfill('0') << millisecondPart.count() << 'Z';
    return output.str();
}

void addContextField(
    foundation::Value::Object& output,
    const char* name,
    const std::optional<std::string>& value)
{
    if(value.has_value()) {
        output.emplace(name, foundation::Value {*value});
    }
}

foundation::Value makeStructuredRecord(
    const observability::LogRecord& record,
    const std::string& timestamp)
{
    foundation::Value::Object output {
        {"category", foundation::Value {record.category}},
        {"level", foundation::Value {levelName(record.level)}},
        {"message", foundation::Value {record.message}},
        {"module", foundation::Value {record.module}},
        {"structuredData", foundation::Value {record.structuredData}},
        {"timestamp", foundation::Value {timestamp}},
    };
    addContextField(output, "sessionId", record.context.sessionId);
    addContextField(output, "projectId", record.context.projectId);
    addContextField(output, "commandId", record.context.commandId);
    addContextField(output, "taskId", record.context.taskId);
    addContextField(output, "workflowId", record.context.workflowId);
    addContextField(output, "correlationId", record.context.correlationId);
    addContextField(output, "traceId", record.context.traceId);
    return foundation::Value {std::move(output)};
}

void appendHumanContext(
    std::ostringstream& output,
    const char* name,
    const std::optional<std::string>& value)
{
    if(value.has_value()) {
        output << ' ' << name << '=' << *value;
    }
}

foundation::Error loggingError(const char* code, const char* message, std::string reason)
{
    return foundation::makeError(
        code,
        foundation::ErrorCategory::Infrastructure,
        message,
        foundation::Value {foundation::Value::Object {
            {"backend", foundation::Value {"spdlog"}},
            {"reason", foundation::Value {std::move(reason)}},
        }});
}

foundation::Result<void> invalidOptions(std::string reason)
{
    return foundation::Result<void>::failure(foundation::makeError(
        "Logging.InvalidOptions",
        foundation::ErrorCategory::Validation,
        "The logging adapter options are invalid",
        foundation::Value {foundation::Value::Object {
            {"reason", foundation::Value {std::move(reason)}},
        }}));
}

} // namespace

class SpdlogLogService::Impl final {
public:
    explicit Impl(const SpdlogLogOptions& options)
    {
        std::vector<spdlog::sink_ptr> humanSinks;
        if(options.enableConsole) {
            humanSinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        }
        if(options.rotatingFilePath.has_value()) {
            humanSinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                options.rotatingFilePath->native(),
                options.rotatingFileMaxBytes,
                options.rotatingFileCount,
                false));
        }
        if(!humanSinks.empty()) {
            humanLogger_ = std::make_shared<spdlog::logger>(
                "lasercnc.kernel.human", humanSinks.begin(), humanSinks.end());
            configureLogger(humanLogger_);
        }

        if(options.jsonlFilePath.has_value()) {
            auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                options.jsonlFilePath->native(),
                options.rotatingFileMaxBytes,
                options.rotatingFileCount,
                false);
            jsonlLogger_ = std::make_shared<spdlog::logger>("lasercnc.kernel.jsonl", std::move(sink));
            configureLogger(jsonlLogger_);
        }
    }

    void configureLogger(const std::shared_ptr<spdlog::logger>& logger)
    {
        logger->set_level(spdlog::level::trace);
        logger->set_pattern("%v");
        logger->set_error_handler([this](const std::string& message) { lastBackendError_ = message; });
    }

    std::mutex mutex_;
    std::string lastBackendError_;
    JsonconsAdapter json_;
    std::shared_ptr<spdlog::logger> humanLogger_;
    std::shared_ptr<spdlog::logger> jsonlLogger_;
};

SpdlogLogService::SpdlogLogService(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation))
{
}

SpdlogLogService::~SpdlogLogService() = default;

foundation::Result<std::unique_ptr<SpdlogLogService>> SpdlogLogService::create(
    SpdlogLogOptions options)
{
    if(!options.enableConsole && !options.rotatingFilePath.has_value()
       && !options.jsonlFilePath.has_value()) {
        auto result = invalidOptions("At least one log output must be enabled");
        return foundation::Result<std::unique_ptr<SpdlogLogService>>::failure(
            std::move(result).error());
    }
    if(options.rotatingFilePath.has_value()
       && (options.rotatingFilePath->empty() || detail::containsEmbeddedNull(*options.rotatingFilePath))) {
        auto result = invalidOptions("Human-readable file path must be non-empty and contain no null characters");
        return foundation::Result<std::unique_ptr<SpdlogLogService>>::failure(
            std::move(result).error());
    }
    if(options.jsonlFilePath.has_value()
       && (options.jsonlFilePath->empty() || detail::containsEmbeddedNull(*options.jsonlFilePath))) {
        auto result = invalidOptions("JSONL file path must be non-empty and contain no null characters");
        return foundation::Result<std::unique_ptr<SpdlogLogService>>::failure(
            std::move(result).error());
    }
    if((options.rotatingFilePath.has_value() || options.jsonlFilePath.has_value())
       && (options.rotatingFileMaxBytes == 0U || options.rotatingFileCount == 0U)) {
        auto result = invalidOptions("Rotating file size and retained file count must be non-zero");
        return foundation::Result<std::unique_ptr<SpdlogLogService>>::failure(
            std::move(result).error());
    }
    if(options.rotatingFilePath.has_value() && options.jsonlFilePath.has_value()
       && options.rotatingFilePath->lexically_normal()
              == options.jsonlFilePath->lexically_normal()) {
        auto result = invalidOptions("Human-readable and JSONL outputs must use different files");
        return foundation::Result<std::unique_ptr<SpdlogLogService>>::failure(
            std::move(result).error());
    }

    try {
        auto implementation = std::make_unique<Impl>(options);
        return foundation::Result<std::unique_ptr<SpdlogLogService>>::success(
            std::unique_ptr<SpdlogLogService>(
                new SpdlogLogService(std::move(implementation))));
    } catch(const std::exception& exception) {
        return foundation::Result<std::unique_ptr<SpdlogLogService>>::failure(
            loggingError(
                "Logging.InitializeFailed", "The logging adapter could not be initialized", exception.what()));
    } catch(...) {
        return foundation::Result<std::unique_ptr<SpdlogLogService>>::failure(
            loggingError(
                "Logging.InitializeFailed", "The logging adapter could not be initialized", "Unknown failure"));
    }
}

foundation::Result<void> SpdlogLogService::write(const observability::LogRecord& record)
{
    try {
        const auto timestamp = formatTimestamp(record.timestamp);
        const auto structured = implementation_->json_.serialize(
            makeStructuredRecord(record, timestamp));
        if(!structured.hasValue()) {
            return foundation::Result<void>::failure(foundation::makeError(
                "Logging.SerializationFailed",
                foundation::ErrorCategory::Infrastructure,
                "The structured log record could not be serialized",
                foundation::Value {},
                foundation::Severity::Error,
                std::make_shared<const foundation::Error>(structured.error())));
        }

        std::ostringstream human;
        human << timestamp << " [" << levelName(record.level) << "] [" << record.module << '/'
              << record.category << "] " << record.message;
        appendHumanContext(human, "sessionId", record.context.sessionId);
        appendHumanContext(human, "projectId", record.context.projectId);
        appendHumanContext(human, "commandId", record.context.commandId);
        appendHumanContext(human, "taskId", record.context.taskId);
        appendHumanContext(human, "workflowId", record.context.workflowId);
        appendHumanContext(human, "correlationId", record.context.correlationId);
        appendHumanContext(human, "traceId", record.context.traceId);
        if(!record.structuredData.empty()) {
            human << " data=" << structured.value();
        }

        std::lock_guard lock(implementation_->mutex_);
        implementation_->lastBackendError_.clear();
        const auto level = toSpdlogLevel(record.level);
        if(implementation_->humanLogger_) {
            implementation_->humanLogger_->log(level, "{}", human.str());
        }
        if(implementation_->jsonlLogger_) {
            implementation_->jsonlLogger_->log(level, "{}", structured.value());
        }
        if(!implementation_->lastBackendError_.empty()) {
            return foundation::Result<void>::failure(loggingError(
                "Logging.WriteFailed",
                "The logging backend failed to write a record",
                implementation_->lastBackendError_));
        }
        return foundation::Result<void>::success();
    } catch(const std::exception& exception) {
        return foundation::Result<void>::failure(loggingError(
            "Logging.WriteFailed", "The logging backend failed to write a record", exception.what()));
    } catch(...) {
        return foundation::Result<void>::failure(loggingError(
            "Logging.WriteFailed", "The logging backend failed to write a record", "Unknown failure"));
    }
}

foundation::Result<void> SpdlogLogService::flush()
{
    try {
        std::lock_guard lock(implementation_->mutex_);
        implementation_->lastBackendError_.clear();
        if(implementation_->humanLogger_) {
            implementation_->humanLogger_->flush();
        }
        if(implementation_->jsonlLogger_) {
            implementation_->jsonlLogger_->flush();
        }
        if(!implementation_->lastBackendError_.empty()) {
            return foundation::Result<void>::failure(loggingError(
                "Logging.FlushFailed",
                "The logging backend failed to flush",
                implementation_->lastBackendError_));
        }
        return foundation::Result<void>::success();
    } catch(const std::exception& exception) {
        return foundation::Result<void>::failure(loggingError(
            "Logging.FlushFailed", "The logging backend failed to flush", exception.what()));
    } catch(...) {
        return foundation::Result<void>::failure(loggingError(
            "Logging.FlushFailed", "The logging backend failed to flush", "Unknown failure"));
    }
}

} // namespace lasercnc::infrastructure
