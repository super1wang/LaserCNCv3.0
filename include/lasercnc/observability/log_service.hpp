#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/value.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace lasercnc::observability {

enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warning, Error, Critical };

struct LogContext final {
    std::optional<std::string> sessionId;
    std::optional<std::string> projectId;
    std::optional<std::string> commandId;
    std::optional<std::string> taskId;
    std::optional<std::string> workflowId;
    std::optional<std::string> correlationId;
    std::optional<std::string> traceId;
};

struct LogRecord final {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level{LogLevel::Info};
    std::string module;
    std::string category;
    std::string message;
    LogContext context;
    foundation::Value::Object structuredData;
};

class ILogService {
public:
    virtual ~ILogService() = default;
    [[nodiscard]] virtual foundation::Result<void> write(const LogRecord& record) = 0;
    [[nodiscard]] virtual foundation::Result<void> flush() = 0;
};

} // namespace lasercnc::observability
