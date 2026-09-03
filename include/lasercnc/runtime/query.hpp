#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/schema.hpp>
#include <lasercnc/foundation/value.hpp>
#include <lasercnc/foundation/version.hpp>
#include <lasercnc/kernel/identifiers.hpp>
#include <lasercnc/state/document.hpp>

#include <optional>

namespace lasercnc::runtime {

struct QueryDescriptor final {
    kernel::QueryName name;
    foundation::Version version;
    foundation::Schema arguments;
    foundation::Schema result;
    kernel::CapabilityId capability;
    bool requiresDocument{true};
    bool deterministic{false};
};

struct QueryRequest final {
    kernel::RequestId requestId;
    kernel::SessionId sessionId;
    kernel::ProjectId projectId;
    std::optional<kernel::DocumentId> documentId;
    kernel::QueryName query;
    foundation::Value arguments;
    kernel::CorrelationId correlationId;
    kernel::TraceId traceId;
};

struct QueryContext final {
    std::optional<state::Document> document;
};

class IQueryHandler {
public:
    virtual ~IQueryHandler() = default;

    [[nodiscard]] virtual foundation::Result<foundation::Value> execute(
        const QueryRequest& request,
        const QueryContext& context) = 0;
};

} // namespace lasercnc::runtime
