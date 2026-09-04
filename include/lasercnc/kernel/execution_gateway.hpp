#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/kernel/module_runtime.hpp>
#include <lasercnc/runtime/command.hpp>
#include <lasercnc/runtime/query.hpp>
#include <lasercnc/runtime/script.hpp>
#include <lasercnc/runtime/task.hpp>
#include <lasercnc/runtime/workflow.hpp>

#include <chrono>
#include <vector>

namespace lasercnc::runtime {
class CommandRegistry;
class CommandRuntime;
class QueryRegistry;
class QueryRuntime;
class ScriptRegistry;
class ScriptRuntime;
class TaskRegistry;
class TaskRuntime;
class WorkflowRegistry;
class WorkflowRuntime;
}

namespace lasercnc::kernel {

class AppKernel;
class ExecutionAdmission;

struct ExecutionCatalog final {
    std::vector<ModuleSnapshot> modules;
    std::vector<runtime::CommandDescriptor> commands;
    std::vector<runtime::QueryDescriptor> queries;
    std::vector<runtime::TaskDescriptor> tasks;
    std::vector<runtime::WorkflowDescriptor> workflows;
    std::vector<runtime::ScriptDescriptor> scripts;
    std::vector<state::ObjectTypeDescriptor> objectTypes;
};

class ExecutionGateway final {
public:
    ExecutionGateway(const ExecutionGateway&) = delete;
    ExecutionGateway& operator=(const ExecutionGateway&) = delete;

    [[nodiscard]] foundation::Result<runtime::CommandResponse> executeCommand(
        runtime::CommandRequest request);
    [[nodiscard]] foundation::Result<runtime::QueryResponse> executeQuery(
        runtime::QueryRequest request);

    [[nodiscard]] foundation::Result<runtime::WorkflowSnapshot> startWorkflow(
        runtime::WorkflowRequest request);
    [[nodiscard]] foundation::Result<runtime::WorkflowSnapshot> advanceWorkflow(
        const WorkflowId& workflowId);
    [[nodiscard]] foundation::Result<runtime::WorkflowSnapshot> cancelWorkflow(
        const WorkflowId& workflowId);
    [[nodiscard]] foundation::Result<runtime::WorkflowSnapshot> workflow(
        const WorkflowId& workflowId) const;

    [[nodiscard]] foundation::Result<runtime::ScriptSnapshot> executeScript(
        runtime::ScriptRequest request);
    [[nodiscard]] foundation::Result<runtime::ScriptSnapshot> advanceScript(
        const ScriptExecutionId& executionId);
    [[nodiscard]] foundation::Result<runtime::ScriptSnapshot> cancelScript(
        const ScriptExecutionId& executionId);
    [[nodiscard]] foundation::Result<runtime::ScriptSnapshot> script(
        const ScriptExecutionId& executionId) const;

    [[nodiscard]] foundation::Result<runtime::TaskSnapshot> task(
        const TaskId& taskId) const;
    [[nodiscard]] foundation::Result<runtime::TaskSnapshot> waitTask(
        const TaskId& taskId,
        std::chrono::milliseconds timeout) const;
    [[nodiscard]] foundation::Result<void> cancelTask(const TaskId& taskId);

    [[nodiscard]] ExecutionCatalog catalog() const;

private:
    friend class AppKernel;

    ExecutionGateway(
        ExecutionAdmission& admission,
        const ModuleRuntime& modules,
        const runtime::CommandRegistry& commandRegistry,
        const runtime::QueryRegistry& queryRegistry,
        const runtime::TaskRegistry& taskRegistry,
        const runtime::WorkflowRegistry& workflowRegistry,
        const runtime::ScriptRegistry& scriptRegistry,
        const state::ObjectTypeRegistry& objectTypes,
        runtime::CommandRuntime& commands,
        runtime::QueryRuntime& queries,
        runtime::TaskRuntime& tasks,
        runtime::WorkflowRuntime& workflows,
        runtime::ScriptRuntime& scripts) noexcept;

    ExecutionAdmission& admission_;
    const ModuleRuntime& modules_;
    const runtime::CommandRegistry& commandRegistry_;
    const runtime::QueryRegistry& queryRegistry_;
    const runtime::TaskRegistry& taskRegistry_;
    const runtime::WorkflowRegistry& workflowRegistry_;
    const runtime::ScriptRegistry& scriptRegistry_;
    const state::ObjectTypeRegistry& objectTypes_;
    runtime::CommandRuntime& commands_;
    runtime::QueryRuntime& queries_;
    runtime::TaskRuntime& tasks_;
    runtime::WorkflowRuntime& workflows_;
    runtime::ScriptRuntime& scripts_;
};

} // namespace lasercnc::kernel
