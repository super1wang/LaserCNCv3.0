#pragma once

#include <lasercnc/observability/log_service.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>

namespace lasercnc::infrastructure {

struct SpdlogLogOptions final {
    bool enableConsole{true};
    std::optional<std::filesystem::path> rotatingFilePath;
    std::optional<std::filesystem::path> jsonlFilePath;
    std::size_t rotatingFileMaxBytes{5U * 1024U * 1024U};
    std::size_t rotatingFileCount{3U};
};

class SpdlogLogService final : public observability::ILogService {
public:
    [[nodiscard]] static foundation::Result<std::unique_ptr<SpdlogLogService>> create(
        SpdlogLogOptions options = {});

    ~SpdlogLogService() override;

    SpdlogLogService(const SpdlogLogService&) = delete;
    SpdlogLogService& operator=(const SpdlogLogService&) = delete;
    SpdlogLogService(SpdlogLogService&&) = delete;
    SpdlogLogService& operator=(SpdlogLogService&&) = delete;

    [[nodiscard]] foundation::Result<void> write(
        const observability::LogRecord& record) override;
    [[nodiscard]] foundation::Result<void> flush() override;

private:
    class Impl;

    explicit SpdlogLogService(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace lasercnc::infrastructure
