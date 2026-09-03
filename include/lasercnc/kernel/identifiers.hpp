#pragma once

#include <lasercnc/foundation/strong_id.hpp>

namespace lasercnc::kernel {

struct ServiceIdTag;
struct ModuleIdTag;
struct CommandNameTag;
struct QueryNameTag;
struct TaskNameTag;
struct TaskIdTag;
struct ResourceIdTag;
struct EventNameTag;
struct CapabilityIdTag;
struct ProjectIdTag;
struct DocumentIdTag;
struct ObjectIdTag;
struct ObjectTypeIdTag;
struct TransactionIdTag;
struct RequestIdTag;
struct SessionIdTag;
struct CorrelationIdTag;
struct TraceIdTag;
struct SpanIdTag;
struct MetricNameTag;
struct DiagnosticIdTag;
struct SnapshotIdTag;
struct ContentDigestTag;
struct IdempotencyKeyTag;
struct SubscriptionIdTag;

using ServiceId = foundation::StrongId<ServiceIdTag>;
using ModuleId = foundation::StrongId<ModuleIdTag>;
using CommandName = foundation::StrongId<CommandNameTag>;
using QueryName = foundation::StrongId<QueryNameTag>;
using TaskName = foundation::StrongId<TaskNameTag>;
using TaskId = foundation::StrongId<TaskIdTag>;
using ResourceId = foundation::StrongId<ResourceIdTag>;
using EventName = foundation::StrongId<EventNameTag>;
using CapabilityId = foundation::StrongId<CapabilityIdTag>;
using ProjectId = foundation::StrongId<ProjectIdTag>;
using DocumentId = foundation::StrongId<DocumentIdTag>;
using ObjectId = foundation::StrongId<ObjectIdTag>;
using ObjectTypeId = foundation::StrongId<ObjectTypeIdTag>;
using TransactionId = foundation::StrongId<TransactionIdTag>;
using RequestId = foundation::StrongId<RequestIdTag>;
using SessionId = foundation::StrongId<SessionIdTag>;
using CorrelationId = foundation::StrongId<CorrelationIdTag>;
using TraceId = foundation::StrongId<TraceIdTag>;
using SpanId = foundation::StrongId<SpanIdTag>;
using MetricName = foundation::StrongId<MetricNameTag>;
using DiagnosticId = foundation::StrongId<DiagnosticIdTag>;
using SnapshotId = foundation::StrongId<SnapshotIdTag>;
using ContentDigest = foundation::StrongId<ContentDigestTag>;
using IdempotencyKey = foundation::StrongId<IdempotencyKeyTag>;
using SubscriptionId = foundation::StrongId<SubscriptionIdTag>;

} // namespace lasercnc::kernel
