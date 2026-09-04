#pragma once
#include <lasercnc/infrastructure/spdlog_log_service.hpp>

namespace lasercnc::infrastructure::detail {
[[nodiscard]] foundation::Result<void> validateLogFileIdentities(const SpdlogLogOptions& options);
}
