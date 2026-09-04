#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/value.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lasercnc::platform {

using PersistenceRow = foundation::Value::Object;

struct PersistenceSessionInfo final {
    std::string backend;
    bool persistent{false};
    foundation::Value configuration;
};

class IPersistenceBackend {
public:
    virtual ~IPersistenceBackend() = default;
    // Acquire exclusive Host ownership and verify the backend session policy before recovery.
    // 中文翻译：恢复前取得独占 Host 所有权并验证连接策略；成功后保留至后端销毁，无提前释放入口。
    [[nodiscard]] virtual foundation::Result<PersistenceSessionInfo> acquireHostSession() = 0;
    [[nodiscard]] virtual foundation::Result<std::size_t> execute(
        std::string_view statement,
        std::span<const foundation::Value> parameters = {}) = 0;
    [[nodiscard]] virtual foundation::Result<std::vector<PersistenceRow>> query(
        std::string_view statement,
        std::span<const foundation::Value> parameters = {}) = 0;
    [[nodiscard]] virtual foundation::Result<void> beginTransaction() = 0;
    [[nodiscard]] virtual foundation::Result<void> commitTransaction() = 0;
    [[nodiscard]] virtual foundation::Result<void> rollbackTransaction() = 0;
};

} // namespace lasercnc::platform
