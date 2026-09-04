#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/runtime/query.hpp>

#include <cstddef>
#include <memory>

namespace lasercnc::kernel {
class AppKernel;
class ExecutionGateway;
}

namespace lasercnc::state {
class DocumentStore;
}

namespace lasercnc::observability {
class IMetricsService;
class ITraceService;
}

namespace lasercnc::runtime {

class CapabilityService;
class ExecutionServices;
class DocumentRuntime;
class QueryRegistry;

class QueryRuntime final {
public:
    QueryRuntime(
        QueryRegistry& registry,
        state::DocumentStore& documents,
        CapabilityService& capabilities,
        ExecutionServices& executionServices,
        observability::ITraceService& traces,
        observability::IMetricsService& metrics,
        DocumentRuntime* documentRuntime = nullptr);
    ~QueryRuntime();

    QueryRuntime(const QueryRuntime&) = delete;
    QueryRuntime& operator=(const QueryRuntime&) = delete;

    [[nodiscard]] foundation::Result<QueryResponse> execute(const QueryRequest& request);
    [[nodiscard]] std::size_t activeExecutionCount() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;

private:
    friend class kernel::AppKernel;
    friend class kernel::ExecutionGateway;

    class Impl;

    void start() noexcept;
    void stop() noexcept;
    [[nodiscard]] foundation::Result<QueryResponse> executeObserved(
        const QueryRequest& request, bool kernelRejected);

    std::unique_ptr<Impl> impl_;
};

} // namespace lasercnc::runtime
