#pragma once

#include <lasercnc/kernel/app_kernel.hpp>
#include <lasercnc/kernel/module_registrar.hpp>

#include <functional>
#include <atomic>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lasercnc::test {

template <typename Id>
Id requiredTestId(const char* value)
{
    auto id = Id::create(value);
    if(!id) {
        throw std::logic_error("Invalid test identity");
    }
    return std::move(id).value();
}

inline foundation::Schema testAnySchema(const char* id)
{
    auto schema = foundation::Schema::create(
        requiredTestId<foundation::SchemaId>(id),
        foundation::Version {1U, 0U, 0U},
        foundation::SchemaKind::Any);
    if(!schema) {
        throw std::logic_error("Invalid test schema");
    }
    return std::move(schema).value();
}

class FixedTaskCommandHandler final : public runtime::IAsyncCommandHandler {
public:
    explicit FixedTaskCommandHandler(runtime::TaskRequest task)
        : task_(std::move(task))
    {
    }

    [[nodiscard]] foundation::Result<runtime::AsyncCommandPlan> prepare(
        const runtime::CommandRequest&) override
    {
        return foundation::Result<runtime::AsyncCommandPlan>::success(
            runtime::AsyncCommandPlan {
                task_, foundation::Value {foundation::Value::Object {
                           {"accepted", foundation::Value {true}}}}});
    }

private:
    runtime::TaskRequest task_;
};

inline runtime::CommandDescriptor taskSubmissionDescriptor(
    const char* commandName,
    const char* capability)
{
    auto descriptor = runtime::CommandDescriptor {
        requiredTestId<kernel::CommandName>(commandName),
        foundation::Version {1U, 0U, 0U},
        testAnySchema("schema.test.task-submit.arguments"),
        testAnySchema("schema.test.task-submit.result"),
        runtime::ExecutionMode::Asynchronous,
        runtime::SideEffectLevel::ReadOnly,
        requiredTestId<kernel::CapabilityId>(capability),
        false,
        true,
        true};
    descriptor.scope = runtime::ExecutionScope::Global;
    return descriptor;
}

inline runtime::CommandRequest taskSubmissionRequest(
    const char* requestId,
    const char* commandName,
    const kernel::SessionId& session,
    const char* traceId)
{
    return runtime::CommandRequest {
        requiredTestId<kernel::RequestId>(requestId),
        runtime::ExecutionContext {session, std::nullopt, std::nullopt},
        requiredTestId<kernel::CommandName>(commandName),
        foundation::Version {1U, 0U, 0U},
        foundation::Value {foundation::Value::Object {}},
        std::nullopt,
        requiredTestId<kernel::CorrelationId>("correlation.test.task-submit"),
        requiredTestId<kernel::TraceId>(traceId)};
}

class KernelTestModule final : public kernel::IModule {
public:
    using Registration = std::function<foundation::Result<void>(kernel::ModuleRegistrar&)>;

    KernelTestModule(kernel::ModuleDescriptor descriptor, std::vector<Registration> registrations)
        : descriptor_(std::move(descriptor)), registrations_(std::move(registrations))
    {
    }

    [[nodiscard]] const kernel::ModuleDescriptor& descriptor() const noexcept override
    {
        return descriptor_;
    }

    [[nodiscard]] foundation::Result<void> registerComponents(
        kernel::ModuleRegistrar& registrar) override
    {
        for(auto& registration : registrations_) {
            auto result = registration(registrar);
            if(!result) {
                return result;
            }
        }
        return foundation::Result<void>::success();
    }

private:
    kernel::ModuleDescriptor descriptor_;
    std::vector<Registration> registrations_;
};

class KernelTestModuleBuilder final {
public:
    explicit KernelTestModuleBuilder(const char* moduleId)
        : descriptor_ {
              makeModuleId(moduleId),
              moduleId,
              foundation::Version {1U, 0U, 0U}}
    {
    }

    KernelTestModuleBuilder& command(
        runtime::CommandDescriptor descriptor,
        std::shared_ptr<runtime::ICommandHandler> handler)
    {
        descriptor_.commands.push_back(runtime::CommandKey {descriptor.name, descriptor.version});
        registrations_.push_back(
            [descriptor = std::move(descriptor), handler = std::move(handler)](
                kernel::ModuleRegistrar& registrar) mutable {
                return registrar.registerCommand(std::move(descriptor), std::move(handler));
            });
        return *this;
    }

    KernelTestModuleBuilder& asyncCommand(
        runtime::CommandDescriptor descriptor,
        std::shared_ptr<runtime::IAsyncCommandHandler> handler)
    {
        descriptor_.commands.push_back(runtime::CommandKey {descriptor.name, descriptor.version});
        registrations_.push_back(
            [descriptor = std::move(descriptor), handler = std::move(handler)](
                kernel::ModuleRegistrar& registrar) mutable {
                return registrar.registerAsyncCommand(std::move(descriptor), std::move(handler));
            });
        return *this;
    }

    KernelTestModuleBuilder& readOnlyCommand(
        runtime::CommandDescriptor descriptor,
        std::shared_ptr<runtime::IReadOnlyCommandHandler> handler)
    {
        descriptor_.commands.push_back(runtime::CommandKey {descriptor.name, descriptor.version});
        registrations_.push_back(
            [descriptor = std::move(descriptor), handler = std::move(handler)](
                kernel::ModuleRegistrar& registrar) mutable {
                return registrar.registerReadOnlyCommand(
                    std::move(descriptor), std::move(handler));
            });
        return *this;
    }

    KernelTestModuleBuilder& externalEffectCommand(
        runtime::CommandDescriptor descriptor,
        std::shared_ptr<runtime::IExternalEffectHandler> handler)
    {
        descriptor_.commands.push_back(runtime::CommandKey {descriptor.name, descriptor.version});
        registrations_.push_back(
            [descriptor = std::move(descriptor), handler = std::move(handler)](
                kernel::ModuleRegistrar& registrar) mutable {
                return registrar.registerExternalEffectCommand(
                    std::move(descriptor), std::move(handler));
            });
        return *this;
    }

    KernelTestModuleBuilder& query(
        runtime::QueryDescriptor descriptor,
        std::shared_ptr<runtime::IQueryHandler> handler)
    {
        descriptor_.queries.push_back(runtime::QueryKey {descriptor.name, descriptor.version});
        registrations_.push_back(
            [descriptor = std::move(descriptor), handler = std::move(handler)](
                kernel::ModuleRegistrar& registrar) mutable {
                return registrar.registerQuery(std::move(descriptor), std::move(handler));
            });
        return *this;
    }

    KernelTestModuleBuilder& task(
        runtime::TaskDescriptor descriptor,
        std::shared_ptr<runtime::ITaskHandler> handler)
    {
        descriptor_.tasks.push_back(descriptor.name);
        registrations_.push_back(
            [descriptor = std::move(descriptor), handler = std::move(handler)](
                kernel::ModuleRegistrar& registrar) mutable {
                return registrar.registerTask(std::move(descriptor), std::move(handler));
            });
        return *this;
    }

    KernelTestModuleBuilder& workflow(runtime::WorkflowDefinition definition)
    {
        descriptor_.workflows.push_back(definition.descriptor.name);
        registrations_.push_back(
            [definition = std::move(definition)](
                kernel::ModuleRegistrar& registrar) mutable {
                return registrar.registerWorkflow(std::move(definition));
            });
        return *this;
    }

    KernelTestModuleBuilder& script(runtime::ScriptDefinition definition)
    {
        descriptor_.scripts.push_back(definition.descriptor.name);
        registrations_.push_back(
            [definition = std::move(definition)](
                kernel::ModuleRegistrar& registrar) mutable {
                return registrar.registerScript(std::move(definition));
            });
        return *this;
    }

    KernelTestModuleBuilder& objectType(state::ObjectTypeDefinition definition)
    {
        descriptor_.objectTypes.push_back(definition.descriptor.type);
        registrations_.push_back(
            [definition = std::move(definition)](
                kernel::ModuleRegistrar& registrar) mutable {
                return registrar.registerObjectType(std::move(definition));
            });
        return *this;
    }

    [[nodiscard]] foundation::Result<void> install(kernel::AppKernel& kernel)
    {
        return kernel.addModule(std::make_unique<KernelTestModule>(
            std::move(descriptor_), std::move(registrations_)));
    }

private:
    static kernel::ModuleId makeModuleId(const char* value)
    {
        auto id = kernel::ModuleId::create(value);
        if(!id) {
            throw std::logic_error("Invalid test module ID");
        }
        return std::move(id).value();
    }

    kernel::ModuleDescriptor descriptor_;
    std::vector<KernelTestModule::Registration> registrations_;
};

inline std::string nextKernelTestModuleId()
{
    static std::atomic<std::uint64_t> sequence {0U};
    std::ostringstream stream;
    stream << "test.contribution." << std::setw(10) << std::setfill('0')
           << sequence.fetch_add(1U, std::memory_order_relaxed);
    return stream.str();
}

template <typename Configure>
foundation::Result<void> installKernelTestModule(
    kernel::AppKernel& kernel,
    Configure&& configure)
{
    const auto id = nextKernelTestModuleId();
    KernelTestModuleBuilder builder(id.c_str());
    std::forward<Configure>(configure)(builder);
    return builder.install(kernel);
}

inline foundation::Result<void> registerCommand(
    kernel::AppKernel& kernel,
    runtime::CommandDescriptor descriptor,
    std::shared_ptr<runtime::ICommandHandler> handler)
{
    return installKernelTestModule(
        kernel,
        [descriptor = std::move(descriptor), handler = std::move(handler)](
            KernelTestModuleBuilder& builder) mutable {
            builder.command(std::move(descriptor), std::move(handler));
        });
}

inline foundation::Result<void> registerAsyncCommand(
    kernel::AppKernel& kernel,
    runtime::CommandDescriptor descriptor,
    std::shared_ptr<runtime::IAsyncCommandHandler> handler)
{
    return installKernelTestModule(
        kernel,
        [descriptor = std::move(descriptor), handler = std::move(handler)](
            KernelTestModuleBuilder& builder) mutable {
            builder.asyncCommand(std::move(descriptor), std::move(handler));
        });
}

inline foundation::Result<void> registerReadOnlyCommand(
    kernel::AppKernel& kernel,
    runtime::CommandDescriptor descriptor,
    std::shared_ptr<runtime::IReadOnlyCommandHandler> handler)
{
    return installKernelTestModule(
        kernel,
        [descriptor = std::move(descriptor), handler = std::move(handler)](
            KernelTestModuleBuilder& builder) mutable {
            builder.readOnlyCommand(std::move(descriptor), std::move(handler));
        });
}

inline foundation::Result<void> registerExternalEffectCommand(
    kernel::AppKernel& kernel,
    runtime::CommandDescriptor descriptor,
    std::shared_ptr<runtime::IExternalEffectHandler> handler)
{
    return installKernelTestModule(
        kernel,
        [descriptor = std::move(descriptor), handler = std::move(handler)](
            KernelTestModuleBuilder& builder) mutable {
            builder.externalEffectCommand(std::move(descriptor), std::move(handler));
        });
}

inline foundation::Result<void> registerQuery(
    kernel::AppKernel& kernel,
    runtime::QueryDescriptor descriptor,
    std::shared_ptr<runtime::IQueryHandler> handler)
{
    return installKernelTestModule(
        kernel,
        [descriptor = std::move(descriptor), handler = std::move(handler)](
            KernelTestModuleBuilder& builder) mutable {
            builder.query(std::move(descriptor), std::move(handler));
        });
}

inline foundation::Result<void> registerTask(
    kernel::AppKernel& kernel,
    runtime::TaskDescriptor descriptor,
    std::shared_ptr<runtime::ITaskHandler> handler)
{
    return installKernelTestModule(
        kernel,
        [descriptor = std::move(descriptor), handler = std::move(handler)](
            KernelTestModuleBuilder& builder) mutable {
            builder.task(std::move(descriptor), std::move(handler));
        });
}

inline foundation::Result<void> registerWorkflow(
    kernel::AppKernel& kernel,
    runtime::WorkflowDefinition definition)
{
    return installKernelTestModule(
        kernel,
        [definition = std::move(definition)](KernelTestModuleBuilder& builder) mutable {
            builder.workflow(std::move(definition));
        });
}

inline foundation::Result<void> registerScript(
    kernel::AppKernel& kernel,
    runtime::ScriptDefinition definition)
{
    return installKernelTestModule(
        kernel,
        [definition = std::move(definition)](KernelTestModuleBuilder& builder) mutable {
            builder.script(std::move(definition));
        });
}

inline foundation::Result<void> registerObjectType(
    kernel::AppKernel& kernel,
    state::ObjectTypeDefinition definition)
{
    return installKernelTestModule(
        kernel,
        [definition = std::move(definition)](KernelTestModuleBuilder& builder) mutable {
            builder.objectType(std::move(definition));
        });
}

} // namespace lasercnc::test
