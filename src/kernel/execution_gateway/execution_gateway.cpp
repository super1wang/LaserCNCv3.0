#include <lasercnc/kernel/execution_gateway.hpp>
#include "../execution_admission.hpp"

#include <lasercnc/runtime/command_registry.hpp>
#include <lasercnc/runtime/command_runtime.hpp>
#include <lasercnc/runtime/query_registry.hpp>
#include <lasercnc/runtime/query_runtime.hpp>
#include <lasercnc/runtime/script_registry.hpp>
#include <lasercnc/runtime/script_runtime.hpp>
#include <lasercnc/runtime/task_registry.hpp>
#include <lasercnc/runtime/task_runtime.hpp>
#include <lasercnc/runtime/workflow_registry.hpp>
#include <lasercnc/runtime/workflow_runtime.hpp>

#include <utility>

namespace lasercnc::kernel {

ExecutionGateway::ExecutionGateway(
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
    runtime::ScriptRuntime& scripts) noexcept
    : admission_(admission),
      modules_(modules),
      commandRegistry_(commandRegistry),
      queryRegistry_(queryRegistry),
      taskRegistry_(taskRegistry),
      workflowRegistry_(workflowRegistry),
      scriptRegistry_(scriptRegistry),
      objectTypes_(objectTypes),
      commands_(commands),
      queries_(queries),
      tasks_(tasks),
      workflows_(workflows),
      scripts_(scripts)
{
}

foundation::Result<runtime::CommandResponse> ExecutionGateway::executeCommand(
    runtime::CommandRequest request)
{
    auto admitted = ExecutionAdmission::acquire(&admission_, "Command.RuntimeNotReady");
    // Preserve rejected-request diagnostics without entering resolution or execution.
    // 中文翻译：保留拒绝请求的观测，不允许进入解析、活动登记或执行路径。
    if(!admitted) { return commands_.executeObserved(request, true); }
    return commands_.execute(std::move(request));
}

foundation::Result<runtime::QueryResponse> ExecutionGateway::executeQuery(
    runtime::QueryRequest request)
{
    auto admitted = ExecutionAdmission::acquire(&admission_, "Query.RuntimeNotReady");
    if(!admitted) { return queries_.executeObserved(request, true); }
    return queries_.execute(std::move(request));
}

foundation::Result<runtime::WorkflowSnapshot> ExecutionGateway::startWorkflow(
    runtime::WorkflowRequest request)
{
    auto admitted = ExecutionAdmission::acquire(&admission_, "Workflow.RuntimeNotAccepting");
    if(!admitted) { return foundation::Result<runtime::WorkflowSnapshot>::failure(std::move(admitted).error()); }
    return workflows_.startWorkflow(std::move(request));
}

foundation::Result<runtime::WorkflowSnapshot> ExecutionGateway::advanceWorkflow(
    const WorkflowId& workflowId)
{
    auto admitted = ExecutionAdmission::acquire(&admission_, "Workflow.RuntimeNotAccepting");
    if(!admitted) { return foundation::Result<runtime::WorkflowSnapshot>::failure(std::move(admitted).error()); }
    return workflows_.advance(workflowId);
}

foundation::Result<runtime::WorkflowSnapshot> ExecutionGateway::cancelWorkflow(
    const WorkflowId& workflowId)
{
    auto admitted = ExecutionAdmission::acquire(&admission_, "Workflow.RuntimeNotAccepting");
    if(!admitted) { return foundation::Result<runtime::WorkflowSnapshot>::failure(std::move(admitted).error()); }
    return workflows_.cancel(workflowId);
}

foundation::Result<runtime::WorkflowSnapshot> ExecutionGateway::workflow(
    const WorkflowId& workflowId) const
{
    return workflows_.snapshot(workflowId);
}

foundation::Result<runtime::ScriptSnapshot> ExecutionGateway::executeScript(
    runtime::ScriptRequest request)
{
    auto admitted = ExecutionAdmission::acquire(&admission_, "Script.RuntimeNotAccepting");
    if(!admitted) { return foundation::Result<runtime::ScriptSnapshot>::failure(std::move(admitted).error()); }
    return scripts_.startScript(std::move(request));
}

foundation::Result<runtime::ScriptSnapshot> ExecutionGateway::advanceScript(
    const ScriptExecutionId& executionId)
{
    auto admitted = ExecutionAdmission::acquire(&admission_, "Script.RuntimeNotAccepting");
    if(!admitted) { return foundation::Result<runtime::ScriptSnapshot>::failure(std::move(admitted).error()); }
    return scripts_.advance(executionId);
}

foundation::Result<runtime::ScriptSnapshot> ExecutionGateway::cancelScript(
    const ScriptExecutionId& executionId)
{
    auto admitted = ExecutionAdmission::acquire(&admission_, "Script.RuntimeNotAccepting");
    if(!admitted) { return foundation::Result<runtime::ScriptSnapshot>::failure(std::move(admitted).error()); }
    return scripts_.cancel(executionId);
}

foundation::Result<runtime::ScriptSnapshot> ExecutionGateway::script(
    const ScriptExecutionId& executionId) const
{
    return scripts_.snapshot(executionId);
}

foundation::Result<runtime::TaskSnapshot> ExecutionGateway::task(
    const TaskId& taskId) const
{
    return tasks_.snapshot(taskId);
}

foundation::Result<runtime::TaskSnapshot> ExecutionGateway::waitTask(
    const TaskId& taskId,
    std::chrono::milliseconds timeout) const
{
    return tasks_.wait(taskId, timeout);
}

foundation::Result<void> ExecutionGateway::cancelTask(const TaskId& taskId)
{
    return tasks_.cancel(taskId);
}

ExecutionCatalog ExecutionGateway::catalog() const
{
    return ExecutionCatalog {
        modules_.snapshot(),
        commandRegistry_.descriptors(),
        queryRegistry_.descriptors(),
        taskRegistry_.descriptors(),
        workflowRegistry_.descriptors(),
        scriptRegistry_.descriptors(),
        objectTypes_.descriptors()};
}

} // namespace lasercnc::kernel
