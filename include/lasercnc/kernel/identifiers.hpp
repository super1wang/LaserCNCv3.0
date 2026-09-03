#pragma once

#include <lasercnc/foundation/strong_id.hpp>

namespace lasercnc::kernel {

struct ServiceIdTag;
struct ModuleIdTag;
struct CommandNameTag;
struct QueryNameTag;
struct TaskNameTag;
struct EventNameTag;
struct CapabilityIdTag;
struct ProjectIdTag;
struct DocumentIdTag;
struct ObjectIdTag;
struct ObjectTypeIdTag;
struct TransactionIdTag;

using ServiceId = foundation::StrongId<ServiceIdTag>;
using ModuleId = foundation::StrongId<ModuleIdTag>;
using CommandName = foundation::StrongId<CommandNameTag>;
using QueryName = foundation::StrongId<QueryNameTag>;
using TaskName = foundation::StrongId<TaskNameTag>;
using EventName = foundation::StrongId<EventNameTag>;
using CapabilityId = foundation::StrongId<CapabilityIdTag>;
using ProjectId = foundation::StrongId<ProjectIdTag>;
using DocumentId = foundation::StrongId<DocumentIdTag>;
using ObjectId = foundation::StrongId<ObjectIdTag>;
using ObjectTypeId = foundation::StrongId<ObjectTypeIdTag>;
using TransactionId = foundation::StrongId<TransactionIdTag>;

} // namespace lasercnc::kernel
