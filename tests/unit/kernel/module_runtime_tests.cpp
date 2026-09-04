#include <lasercnc/kernel/app_kernel.hpp>

#include <catch2/catch_test_macros.hpp>
#include "kernel_test_module.hpp"

#include <algorithm>
#include <memory>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace lasercnc::foundation;
using namespace lasercnc::kernel;
using namespace lasercnc::observability;
using namespace lasercnc::platform;
using namespace lasercnc::runtime;

static_assert(std::is_same_v<
              decltype(std::declval<AppKernel&>().services()),
              const ServiceRegistry&>);
static_assert(std::is_same_v<
              decltype(std::declval<AppKernel&>().modules()),
              const ModuleRuntime&>);
static_assert(std::is_same_v<
              decltype(std::declval<AppKernel&>().commandRegistry()),
              const CommandRegistry&>);
static_assert(std::is_same_v<
              decltype(std::declval<AppKernel&>().queryRegistry()),
              const QueryRegistry&>);
static_assert(std::is_same_v<
              decltype(std::declval<AppKernel&>().taskRegistry()),
              const TaskRegistry&>);
static_assert(std::is_same_v<
              decltype(std::declval<AppKernel&>().workflowRegistry()),
              const WorkflowRegistry&>);
static_assert(std::is_same_v<
              decltype(std::declval<AppKernel&>().scriptRegistry()),
              const ScriptRegistry&>);
static_assert(std::is_same_v<decltype(std::declval<AppKernel&>().history()), const HistoryRuntime&>);
static_assert(std::is_same_v<decltype(std::declval<AppKernel&>().persistence()),
              const lasercnc::persistence::PersistenceService&>);

template<typename Host>
concept HostRestoresHistory = requires(Host& host, std::span<const TransactionCommit> commits) {
    host.history().restore(commits);
};
template<typename Host>
concept HostAppendsJournal = requires(Host& host, const TransactionCommit& commit) {
    host.persistence().append(commit);
};
template<typename Host>
concept HostAcceptsTask = requires(Host& host, const TaskRequest& request) {
    host.persistence().acceptTask(request, std::nullopt);
};
template<typename Host>
concept HostWritesWorkflow = requires(Host& host, const WorkflowRequest& request,
    const WorkflowDefinition& definition, const WorkflowSnapshot& snapshot) {
    host.persistence().saveWorkflowCheckpoint(request, definition, snapshot, {});
};
template<typename Host>
concept HostInitializesPersistence = requires(Host& host) { host.persistence().initialize(); };
static_assert(!HostRestoresHistory<AppKernel> && !HostRestoresHistory<const AppKernel>);
static_assert(!HostAppendsJournal<AppKernel> && !HostAppendsJournal<const AppKernel>);
static_assert(!HostAcceptsTask<AppKernel> && !HostAcceptsTask<const AppKernel>);
static_assert(!HostWritesWorkflow<AppKernel> && !HostWritesWorkflow<const AppKernel>);
static_assert(!HostInitializesPersistence<AppKernel> && !HostInitializesPersistence<const AppKernel>);
// Positive controls ensure these are real component methods, not vacuous missing-name checks.
// 中文翻译：正对照确认方法在独立组件上真实存在，不因方法名不存在而产生空通过。
static_assert(requires(HistoryRuntime& history, std::span<const TransactionCommit> commits) { history.restore(commits); });
static_assert(requires(lasercnc::persistence::PersistenceService& service, const TransactionCommit& commit,
    const TaskRequest& task, const WorkflowRequest& request, const WorkflowDefinition& definition,
    const WorkflowSnapshot& snapshot) {
    service.append(commit); service.acceptTask(task, std::nullopt);
    service.saveWorkflowCheckpoint(request, definition, snapshot, {}); service.initialize();
});

namespace {

template <typename Id>
Id makeId(const char* value)
{
    auto result = Id::create(value);
    if(!result) {
        throw std::logic_error("Invalid test identity");
    }
    return std::move(result).value();
}

Schema makeSchema(const char* value)
{
    auto result = Schema::create(
        makeId<SchemaId>(value), Version {1U, 0U, 0U}, SchemaKind::Object);
    if(!result) {
        throw std::logic_error("Invalid test schema");
    }
    return std::move(result).value();
}

class PassSchemaValidator final : public ISchemaValidator {
public:
    Result<void> validate(const Schema&, const Value&) const override
    {
        return Result<void>::success();
    }
};

class NullLogService final : public ILogService {
public:
    Result<void> write(const LogRecord&) override { return Result<void>::success(); }
    Result<void> flush() override { return Result<void>::success(); }
};

class NullReadOnlyCommandHandler final : public IReadOnlyCommandHandler {
public:
    Result<Value> execute(const CommandRequest&, const ReadOnlyCommandContext&) override
    {
        return Result<Value>::success(Value {Value::Object {}});
    }
};

class NullQueryHandler final : public IQueryHandler {
public:
    Result<Value> execute(const QueryRequest&, const QueryContext&) override
    {
        return Result<Value>::success(Value {Value::Object {}});
    }
};

class NullTaskHandler final : public ITaskHandler {
public:
    Result<Value> execute(const TaskRequest&, const TaskContext&) override
    {
        return Result<Value>::success(Value {Value::Object {}});
    }
};

class InlineTaskExecutor final : public ITaskExecutor {
public:
    Result<void> submit(ExecutorWork work, ExecutorCompletion completion) override
    {
        completion(work());
        return Result<void>::success();
    }

    Result<void> waitIdle() override { return Result<void>::success(); }
    Result<void> shutdown() override { return Result<void>::success(); }
    std::size_t concurrency() const noexcept override { return 1U; }
};

struct IProbeService {
    virtual ~IProbeService() = default;
    [[nodiscard]] virtual int value() const = 0;
};

class ProbeService final : public IProbeService {
public:
    [[nodiscard]] int value() const override
    {
        return 99;
    }
};

ModuleId makeModuleId(const char* value)
{
    auto result = ModuleId::create(value);
    if(!result) {
        throw std::logic_error("Invalid test module ID");
    }
    return std::move(result).value();
}

ServiceId makeServiceId(const char* value)
{
    auto result = ServiceId::create(value);
    if(!result) {
        throw std::logic_error("Invalid test service ID");
    }
    return std::move(result).value();
}

class TestModule final : public IModule {
public:
    TestModule(
        ModuleDescriptor descriptor,
        std::shared_ptr<std::vector<std::string>> events,
        std::optional<ServiceId> serviceToRegister = std::nullopt,
        bool failStart = false,
        bool registerDeclaredService = true,
        bool failStop = false)
        : descriptor_(std::move(descriptor))
        , events_(std::move(events))
        , serviceToRegister_(std::move(serviceToRegister))
        , failStart_(failStart)
        , registerDeclaredService_(registerDeclaredService)
        , failStop_(failStop)
    {
    }

    [[nodiscard]] const ModuleDescriptor& descriptor() const noexcept override
    {
        return descriptor_;
    }

    [[nodiscard]] Result<void> registerComponents(ModuleRegistrar& registrar) override
    {
        append("register");
        if(serviceToRegister_.has_value() && registerDeclaredService_) {
            std::shared_ptr<IProbeService> service = std::make_shared<ProbeService>();
            return registrar.registerService(*serviceToRegister_, std::move(service));
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> initialize(AppKernel&) override
    {
        append("initialize");
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> start(AppKernel&) override
    {
        append("start");
        if(failStart_) {
            return Result<void>::failure(makeError(
                "Test.StartFailed",
                ErrorCategory::Internal,
                "Injected start failure"));
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> stop(AppKernel&) override
    {
        append("stop");
        if(failStop_) {
            return Result<void>::failure(makeError(
                "Test.StopFailed",
                ErrorCategory::Internal,
                "Injected stop failure"));
        }
        return Result<void>::success();
    }

private:
    void append(const char* stage)
    {
        events_->push_back(std::string(descriptor_.id.value()) + '.' + stage);
    }

    ModuleDescriptor descriptor_;
    std::shared_ptr<std::vector<std::string>> events_;
    std::optional<ServiceId> serviceToRegister_;
    bool failStart_{false};
    bool registerDeclaredService_{true};
    bool failStop_{false};
};

class ThrowingInitializeModule final : public IModule {
public:
    ThrowingInitializeModule(
        ModuleDescriptor descriptor,
        std::shared_ptr<std::vector<std::string>> events)
        : descriptor_(std::move(descriptor))
        , events_(std::move(events))
    {
    }

    [[nodiscard]] const ModuleDescriptor& descriptor() const noexcept override
    {
        return descriptor_;
    }

    [[nodiscard]] Result<void> registerComponents(ModuleRegistrar&) override
    {
        events_->push_back("module.throwing.register");
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> initialize(AppKernel&) override
    {
        events_->push_back("module.throwing.initialize");
        throw std::runtime_error("Injected lifecycle exception");
    }

    [[nodiscard]] Result<void> stop(AppKernel&) override
    {
        events_->push_back("module.throwing.stop");
        return Result<void>::success();
    }

private:
    ModuleDescriptor descriptor_;
    std::shared_ptr<std::vector<std::string>> events_;
};

class RegistrarModule final : public IModule {
public:
    using Registration = std::function<Result<void>(ModuleRegistrar&)>;

    RegistrarModule(
        ModuleDescriptor descriptor,
        Registration registration,
        bool failStart = false)
        : descriptor_(std::move(descriptor)),
          registration_(std::move(registration)),
          failStart_(failStart)
    {
    }

    const ModuleDescriptor& descriptor() const noexcept override
    {
        return descriptor_;
    }

    Result<void> registerComponents(ModuleRegistrar& registrar) override
    {
        return registration_(registrar);
    }

    Result<void> start(AppKernel&) override
    {
        return failStart_
            ? Result<void>::failure(makeError(
                "Test.RegistrarStartFailed",
                ErrorCategory::Internal,
                "Injected registrar module start failure"))
            : Result<void>::success();
    }

private:
    ModuleDescriptor descriptor_;
    Registration registration_;
    bool failStart_{false};
};

ModuleDescriptor makeDescriptor(
    const char* id,
    Version version = Version {1, 0, 0},
    std::vector<ModuleDependency> dependencies = {},
    std::vector<ServiceId> requiredServices = {},
    std::vector<ServiceId> providedServices = {})
{
    return ModuleDescriptor {
        makeModuleId(id),
        id,
        version,
        std::move(dependencies),
        std::move(requiredServices),
        std::move(providedServices)};
}

CommandDescriptor governedCommand(const char* name)
{
    return CommandDescriptor {
        makeId<CommandName>(name),
        Version {1U, 0U, 0U},
        makeSchema("schema.module.command.arguments"),
        makeSchema("schema.module.command.result"),
        ExecutionMode::Synchronous,
        SideEffectLevel::ReadOnly,
        makeId<CapabilityId>("capability.module.execute"),
        false,
        true,
        false,
        ContractStatus::Active,
        ExecutionScope::Global};
}

QueryDescriptor governedQuery(const char* name)
{
    return QueryDescriptor {
        makeId<QueryName>(name),
        Version {1U, 0U, 0U},
        makeSchema("schema.module.query.arguments"),
        makeSchema("schema.module.query.result"),
        makeId<CapabilityId>("capability.module.execute"),
        ExecutionScope::Global,
        true};
}

TaskDescriptor governedTask(const char* name)
{
    return TaskDescriptor {
        makeId<TaskName>(name),
        Version {1U, 0U, 0U},
        makeSchema("schema.module.task.input"),
        makeSchema("schema.module.task.result")};
}

WorkflowDefinition governedWorkflow(const char* name)
{
    return WorkflowDefinition {
        WorkflowDescriptor {
            makeId<WorkflowName>(name),
            Version {1U, 0U, 0U},
            makeSchema("schema.module.workflow.input"),
            makeSchema("schema.module.workflow.result")},
        {},
        Value {Value::Object {}}};
}

ScriptDefinition governedScript(const char* name)
{
    return ScriptDefinition {
        ScriptDescriptor {
            makeId<ScriptName>(name),
            Version {1U, 0U, 0U},
            makeSchema("schema.module.script.input"),
            makeSchema("schema.module.script.result")},
        {},
        Value {Value::Object {}}};
}

ModuleDescriptor governedDescriptor(const char* id)
{
    auto descriptor = makeDescriptor(
        id,
        Version {1U, 0U, 0U},
        {},
        {},
        {makeServiceId("service.module.governed")});
    descriptor.commands = {CommandKey {
        makeId<CommandName>("command.module.governed"), Version {1U, 0U, 0U}}};
    descriptor.queries = {QueryKey {
        makeId<QueryName>("query.module.governed"), Version {1U, 0U, 0U}}};
    descriptor.tasks = {makeId<TaskName>("task.module.governed")};
    descriptor.workflows = {makeId<WorkflowName>("workflow.module.governed")};
    descriptor.scripts = {makeId<ScriptName>("script.module.governed")};
    descriptor.events = {makeId<EventName>("event.module.governed")};
    descriptor.capabilities = {
        makeId<CapabilityId>("capability.module.governed")};
    return descriptor;
}

Result<void> registerGovernedContributionsObserved(ModuleRegistrar& registrar, std::weak_ptr<IProbeService>* observed)
{
    std::shared_ptr<IProbeService> service = std::make_shared<ProbeService>();
    if(observed) { *observed = service; }
    auto result = registrar.registerService(
        makeServiceId("service.module.governed"), std::move(service));
    if(result) {
        result = registrar.registerReadOnlyCommand(
            governedCommand("command.module.governed"),
            std::make_shared<NullReadOnlyCommandHandler>());
    }
    if(result) {
        result = registrar.registerQuery(
            governedQuery("query.module.governed"),
            std::make_shared<NullQueryHandler>());
    }
    if(result) {
        result = registrar.registerTask(
            governedTask("task.module.governed"),
            std::make_shared<NullTaskHandler>());
    }
    if(result) {
        result = registrar.registerWorkflow(
            governedWorkflow("workflow.module.governed"));
    }
    if(result) {
        result = registrar.registerScript(governedScript("script.module.governed"));
    }
    if(result) {
        result = registrar.registerEvent(makeId<EventName>("event.module.governed"));
    }
    if(result) {
        result = registrar.registerCapability(
            makeId<CapabilityId>("capability.module.governed"));
    }
    return result;
}

Result<void> registerGovernedContributions(ModuleRegistrar& registrar)
{
    return registerGovernedContributionsObserved(registrar, nullptr);
}

} // namespace

TEST_CASE("AppKernel state observers cannot write and persistence composition closes at startup", "[kernel][persistence][host-boundary]")
{
    class CompositionProbe final : public IModule {
    public:
        explicit CompositionProbe(bool fail = false) : fail_(fail) {}
        const ModuleDescriptor& descriptor() const noexcept override { return descriptor_; }
        Result<void> initialize(AppKernel& kernel) override
        {
            CHECK(kernel.state() == AppKernelState::Starting);
            const auto rejected = kernel.configurePersistence(nullptr, nullptr, nullptr);
            REQUIRE_FALSE(rejected.hasValue());
            CHECK(std::string(rejected.error().code.value()) == "Kernel.PersistenceConfigurationClosed");
            return fail_ ? Result<void>::failure(makeError("Test.CompositionRejected", ErrorCategory::Internal,
                "Intentional composition failure")) : Result<void>::success();
        }
    private:
        bool fail_;
        ModuleDescriptor descriptor_{makeId<ModuleId>("module.persistence-composition-probe"),
            "Persistence composition probe", {1U, 0U, 0U}};
    };
    AppKernel kernel;
    const AppKernel& observer = kernel;
    CHECK(&kernel.history() == &observer.history());
    CHECK(&kernel.persistence() == &observer.persistence());
    REQUIRE(kernel.addModule(std::make_unique<CompositionProbe>()).hasValue());
    REQUIRE(kernel.bootstrap().hasValue());
    auto ready = kernel.configurePersistence(nullptr, nullptr, nullptr);
    REQUIRE_FALSE(ready.hasValue());
    CHECK(std::string(ready.error().code.value()) == "Kernel.PersistenceConfigurationClosed");
    REQUIRE(kernel.shutdown().hasValue());
    auto stopped = kernel.configurePersistence(nullptr, nullptr, nullptr);
    REQUIRE_FALSE(stopped.hasValue());
    CHECK(std::string(stopped.error().code.value()) == "Kernel.PersistenceConfigurationClosed");
    AppKernel failed;
    REQUIRE(failed.addModule(std::make_unique<CompositionProbe>(true)).hasValue());
    REQUIRE_FALSE(failed.bootstrap().hasValue());
    CHECK(failed.state() == AppKernelState::Failed);
    const auto retry = failed.configurePersistence(nullptr, nullptr, nullptr);
    REQUIRE_FALSE(retry.hasValue());
    CHECK(std::string(retry.error().code.value()) == "Kernel.PersistenceConfigurationClosed");
}

TEST_CASE("Kernel module stress failures release all contributions and owned services", "[kernel][modules][stress][f3b]")
{
    for(unsigned int round = 0U; round < 20U; ++round) {
        for(unsigned int failureMode = 0U; failureMode < 3U; ++failureMode) {
            INFO("round=" << round << " failureMode=" << failureMode);
            std::weak_ptr<IProbeService> lifetime;
            {
                AppKernel kernel;
                REQUIRE(kernel.executionServices().configure(std::make_shared<PassSchemaValidator>(),
                    std::make_shared<NullLogService>()));
                auto descriptor = governedDescriptor("module.stress.provider");
                descriptor.objectTypes = {makeId<ObjectTypeId>("type.stress.module")};
                REQUIRE(kernel.addModule(std::make_unique<RegistrarModule>(descriptor,
                    [&](ModuleRegistrar& registrar) {
                        auto result = registerGovernedContributionsObserved(registrar, &lifetime);
                        if(result) { result = registrar.registerObjectType(lasercnc::test::valueObjectType("type.stress.module")); }
                        if(!result) { return result; }
                        if(failureMode == 0U) {
                            return Result<void>::failure(makeError("Test.StressRegistrationFailed", ErrorCategory::Internal,
                                "Failure after all contributions were installed"));
                        }
                        if(failureMode == 1U) { throw std::runtime_error("Stress registration exception"); }
                        return Result<void>::success();
                    })));
                if(failureMode == 2U) {
                    REQUIRE(kernel.addModule(std::make_unique<RegistrarModule>(makeDescriptor("module.stress.failing",
                        {1U, 0U, 0U}, {{descriptor.id, {1U, 0U, 0U}}}),
                        [](ModuleRegistrar&) { return Result<void>::success(); }, true)));
                }
                const auto started = kernel.bootstrap();
                REQUIRE_FALSE(started);
                REQUIRE(std::string(started.error().code.value()) == "Kernel.ModuleLifecycleFailed");
                REQUIRE(started.error().cause != nullptr);
                if(failureMode == 0U) {
                    REQUIRE(std::string(started.error().cause->code.value()) == "Test.StressRegistrationFailed");
                }
                if(failureMode == 1U) {
                    REQUIRE(std::string(started.error().cause->code.value()) == "Kernel.ModuleLifecycleException");
                }
                if(failureMode == 2U) {
                    REQUIRE(std::string(started.error().cause->code.value()) == "Test.RegistrarStartFailed");
                }
                REQUIRE(kernel.state() == AppKernelState::Failed);
                REQUIRE(lifetime.expired());
                REQUIRE_FALSE(kernel.services().contains(makeServiceId("service.module.governed")));
                REQUIRE(kernel.commandRegistry().size() == 0U);
                REQUIRE(kernel.queryRegistry().size() == 0U);
                REQUIRE(kernel.taskRegistry().size() == 0U);
                REQUIRE(kernel.workflowRegistry().size() == 0U);
                REQUIRE(kernel.scriptRegistry().size() == 0U);
                REQUIRE(kernel.objectTypes().size() == 0U);
            }
            REQUIRE(lifetime.expired());
            // The same declared identities must remain usable in a new composition.
            // 中文翻译：新的内核组合仍能使用相同声明身份，失败不能泄漏进程级所有权。
            AppKernel healthy;
            REQUIRE(healthy.executionServices().configure(std::make_shared<PassSchemaValidator>(),
                std::make_shared<NullLogService>()));
            REQUIRE(healthy.configureTaskExecutor(std::make_unique<InlineTaskExecutor>()));
            auto descriptor = governedDescriptor("module.stress.provider");
            descriptor.objectTypes = {makeId<ObjectTypeId>("type.stress.module")};
            REQUIRE(healthy.addModule(std::make_unique<RegistrarModule>(descriptor,
                [](ModuleRegistrar& registrar) {
                    auto result = registerGovernedContributions(registrar);
                    if(result) { result = registrar.registerObjectType(lasercnc::test::valueObjectType("type.stress.module")); }
                    return result;
                })));
            const auto started = healthy.bootstrap();
            INFO((started ? std::string{} : std::string(started.error().code.value())));
            REQUIRE(started);
            REQUIRE(healthy.commandRegistry().size() > 0U);
            REQUIRE(healthy.queryRegistry().size() == 1U);
            REQUIRE(healthy.objectTypes().size() == 1U);
            REQUIRE(healthy.shutdown());
        }
    }
}

TEST_CASE("ModuleRuntime uses deterministic dependency lifecycle ordering", "[kernel][modules]")
{
    AppKernel kernel;
    auto events = std::make_shared<std::vector<std::string>>();
    const auto serviceId = makeServiceId("service.test.probe");
    const auto providerId = makeModuleId("module.provider");

    auto consumer = std::make_unique<TestModule>(
        makeDescriptor(
            "module.consumer",
            Version {1, 0, 0},
            std::vector<ModuleDependency> {{providerId, Version {1, 0, 0}}},
            std::vector<ServiceId> {serviceId}),
        events);
    auto provider = std::make_unique<TestModule>(
        makeDescriptor(
            "module.provider",
            Version {1, 2, 0},
            {},
            {},
            std::vector<ServiceId> {serviceId}),
        events,
        serviceId);

    REQUIRE(kernel.addModule(std::move(consumer)).hasValue());
    REQUIRE(kernel.addModule(std::move(provider)).hasValue());
    REQUIRE(kernel.bootstrap().hasValue());

    CHECK(*events == std::vector<std::string> {
        "module.provider.register",
        "module.consumer.register",
        "module.provider.initialize",
        "module.consumer.initialize",
        "module.provider.start",
        "module.consumer.start"});
    CHECK(kernel.state() == AppKernelState::Ready);
    CHECK(kernel.modules().state() == ModuleRuntimeState::Ready);
    CHECK(kernel.services().frozen());

    auto service = kernel.services().resolve<IProbeService>(serviceId);
    REQUIRE(service.hasValue());
    CHECK(service.value()->value() == 99);

    for(const auto& snapshot : kernel.modules().snapshot()) {
        CHECK(snapshot.state == ModuleState::Ready);
    }

    REQUIRE(kernel.shutdown().hasValue());
    CHECK(events->at(events->size() - 2) == "module.consumer.stop");
    CHECK(events->back() == "module.provider.stop");
    CHECK(kernel.state() == AppKernelState::Stopped);
}

TEST_CASE("ModuleRuntime rejects missing incompatible and cyclic dependencies", "[kernel][modules]")
{
    SECTION("missing dependency")
    {
        AppKernel kernel;
        auto events = std::make_shared<std::vector<std::string>>();
        auto module = std::make_unique<TestModule>(
            makeDescriptor(
                "module.consumer",
                Version {1, 0, 0},
                std::vector<ModuleDependency> {{makeModuleId("module.missing"), Version {1, 0, 0}}}),
            events);
        REQUIRE(kernel.addModule(std::move(module)).hasValue());

        auto result = kernel.bootstrap();
        REQUIRE_FALSE(result.hasValue());
        CHECK(std::string(result.error().code.value()) == "Kernel.ModuleDependencyMissing");
        CHECK(events->empty());
    }

    SECTION("version conflict")
    {
        AppKernel kernel;
        auto events = std::make_shared<std::vector<std::string>>();
        const auto providerId = makeModuleId("module.provider");
        REQUIRE(kernel.addModule(std::make_unique<TestModule>(
                    makeDescriptor("module.provider", Version {2, 0, 0}), events))
                    .hasValue());
        REQUIRE(kernel.addModule(std::make_unique<TestModule>(
                    makeDescriptor(
                        "module.consumer",
                        Version {1, 0, 0},
                        std::vector<ModuleDependency> {{providerId, Version {1, 0, 0}}}),
                    events))
                    .hasValue());

        auto result = kernel.bootstrap();
        REQUIRE_FALSE(result.hasValue());
        CHECK(std::string(result.error().code.value()) == "Kernel.ModuleDependencyVersionConflict");
        CHECK(events->empty());
    }

    SECTION("dependency cycle")
    {
        AppKernel kernel;
        auto events = std::make_shared<std::vector<std::string>>();
        REQUIRE(kernel.addModule(std::make_unique<TestModule>(
                    makeDescriptor(
                        "module.a",
                        Version {1, 0, 0},
                        std::vector<ModuleDependency> {{makeModuleId("module.b"), Version {1, 0, 0}}}),
                    events))
                    .hasValue());
        REQUIRE(kernel.addModule(std::make_unique<TestModule>(
                    makeDescriptor(
                        "module.b",
                        Version {1, 0, 0},
                        std::vector<ModuleDependency> {{makeModuleId("module.a"), Version {1, 0, 0}}}),
                    events))
                    .hasValue());

        auto result = kernel.bootstrap();
        REQUIRE_FALSE(result.hasValue());
        CHECK(std::string(result.error().code.value()) == "Kernel.ModuleDependencyCycle");
        CHECK(events->empty());
    }
}

TEST_CASE("ModuleRuntime rolls back services and modules after start failure", "[kernel][modules]")
{
    AppKernel kernel;
    auto events = std::make_shared<std::vector<std::string>>();
    const auto serviceId = makeServiceId("service.test.rollback");
    const auto providerId = makeModuleId("module.provider");

    REQUIRE(kernel.addModule(std::make_unique<TestModule>(
                makeDescriptor(
                    "module.provider",
                    Version {1, 0, 0},
                    {},
                    {},
                    std::vector<ServiceId> {serviceId}),
                events,
                serviceId))
                .hasValue());
    REQUIRE(kernel.addModule(std::make_unique<TestModule>(
                makeDescriptor(
                    "module.failing",
                    Version {1, 0, 0},
                    std::vector<ModuleDependency> {{providerId, Version {1, 0, 0}}},
                    std::vector<ServiceId> {serviceId}),
                events,
                std::nullopt,
                true))
                .hasValue());

    auto result = kernel.bootstrap();
    REQUIRE_FALSE(result.hasValue());
    CHECK(std::string(result.error().code.value()) == "Kernel.ModuleLifecycleFailed");
    REQUIRE(result.error().cause != nullptr);
    CHECK(std::string(result.error().cause->code.value()) == "Test.StartFailed");
    CHECK_FALSE(kernel.services().contains(serviceId));
    CHECK_FALSE(kernel.services().frozen());
    CHECK(kernel.state() == AppKernelState::Failed);

    CHECK(*events == std::vector<std::string> {
        "module.provider.register",
        "module.failing.register",
        "module.provider.initialize",
        "module.failing.initialize",
        "module.provider.start",
        "module.failing.start",
        "module.failing.stop",
        "module.provider.stop"});

    const auto snapshots = kernel.modules().snapshot();
    REQUIRE(snapshots.size() == 2);
    CHECK(snapshots[0].state == ModuleState::Stopped);
    CHECK(snapshots[1].state == ModuleState::Failed);
}

TEST_CASE("ModuleRuntime enforces declared service composition", "[kernel][modules]")
{
    AppKernel kernel;
    auto events = std::make_shared<std::vector<std::string>>();
    const auto serviceId = makeServiceId("service.test.declared");
    REQUIRE(kernel.addModule(std::make_unique<TestModule>(
                makeDescriptor(
                    "module.incomplete",
                    Version {1, 0, 0},
                    {},
                    {},
                    std::vector<ServiceId> {serviceId}),
                events,
                serviceId,
                false,
                false))
                .hasValue());

    auto result = kernel.bootstrap();
    REQUIRE_FALSE(result.hasValue());
    CHECK(std::string(result.error().code.value())
          == "Kernel.ModuleContributionDeclarationMismatch");
    CHECK_FALSE(kernel.services().contains(serviceId));
    CHECK(*events == std::vector<std::string> {
        "module.incomplete.register",
        "module.incomplete.stop"});
}

TEST_CASE("ModuleRuntime rejects conflicting service providers before callbacks", "[kernel][modules]")
{
    AppKernel kernel;
    auto events = std::make_shared<std::vector<std::string>>();
    const auto serviceId = makeServiceId("service.test.conflict");
    const auto firstDescriptor = makeDescriptor(
        "module.first",
        Version {1, 0, 0},
        {},
        {},
        std::vector<ServiceId> {serviceId});
    const auto secondDescriptor = makeDescriptor(
        "module.second",
        Version {1, 0, 0},
        {},
        {},
        std::vector<ServiceId> {serviceId});

    REQUIRE(kernel.addModule(std::make_unique<TestModule>(firstDescriptor, events, serviceId)).hasValue());
    REQUIRE(kernel.addModule(std::make_unique<TestModule>(secondDescriptor, events, serviceId)).hasValue());

    auto result = kernel.bootstrap();
    REQUIRE_FALSE(result.hasValue());
    CHECK(std::string(result.error().code.value())
          == "Kernel.ModuleContributionOwnerConflict");
    CHECK(events->empty());
}

TEST_CASE("ModuleRuntime reports rollback failure after completing cleanup", "[kernel][modules]")
{
    AppKernel kernel;
    auto events = std::make_shared<std::vector<std::string>>();
    const auto serviceId = makeServiceId("service.test.rollback-failure");
    const auto providerId = makeModuleId("module.provider");

    REQUIRE(kernel.addModule(std::make_unique<TestModule>(
                makeDescriptor(
                    "module.provider",
                    Version {1, 0, 0},
                    {},
                    {},
                    std::vector<ServiceId> {serviceId}),
                events,
                serviceId,
                false,
                true,
                true))
                .hasValue());
    REQUIRE(kernel.addModule(std::make_unique<TestModule>(
                makeDescriptor(
                    "module.failing",
                    Version {1, 0, 0},
                    std::vector<ModuleDependency> {{providerId, Version {1, 0, 0}}},
                    std::vector<ServiceId> {serviceId}),
                events,
                std::nullopt,
                true))
                .hasValue());

    auto result = kernel.bootstrap();
    REQUIRE_FALSE(result.hasValue());
    CHECK(std::string(result.error().code.value()) == "Kernel.ModuleRollbackFailed");
    REQUIRE(result.error().cause != nullptr);
    CHECK(std::string(result.error().cause->code.value()) == "Kernel.ModuleLifecycleFailed");
    CHECK_FALSE(kernel.services().contains(serviceId));
    CHECK(events->at(events->size() - 2) == "module.failing.stop");
    CHECK(events->back() == "module.provider.stop");
}

TEST_CASE("ModuleRuntime converts lifecycle exceptions and rolls back", "[kernel][modules]")
{
    AppKernel kernel;
    auto events = std::make_shared<std::vector<std::string>>();
    REQUIRE(kernel.addModule(std::make_unique<ThrowingInitializeModule>(
                makeDescriptor("module.throwing"),
                events))
                .hasValue());

    auto result = kernel.bootstrap();
    REQUIRE_FALSE(result.hasValue());
    CHECK(std::string(result.error().code.value()) == "Kernel.ModuleLifecycleFailed");
    REQUIRE(result.error().cause != nullptr);
    CHECK(std::string(result.error().cause->code.value()) == "Kernel.ModuleLifecycleException");
    CHECK(*events == std::vector<std::string> {
        "module.throwing.register",
        "module.throwing.initialize",
        "module.throwing.stop"});
}

TEST_CASE("AppKernel rejects invalid composition mutations", "[kernel][modules]")
{
    AppKernel kernel;
    auto events = std::make_shared<std::vector<std::string>>();

    auto nullResult = kernel.addModule(nullptr);
    REQUIRE_FALSE(nullResult.hasValue());
    CHECK(std::string(nullResult.error().code.value()) == "Kernel.ModuleNull");

    auto descriptor = makeDescriptor("module.duplicate");
    REQUIRE(kernel.addModule(std::make_unique<TestModule>(descriptor, events)).hasValue());
    auto duplicate = kernel.addModule(std::make_unique<TestModule>(descriptor, events));
    REQUIRE_FALSE(duplicate.hasValue());
    CHECK(std::string(duplicate.error().code.value()) == "Kernel.ModuleAlreadyRegistered");

    REQUIRE(kernel.bootstrap().hasValue());
    auto lateModule = kernel.addModule(std::make_unique<TestModule>(
        makeDescriptor("module.late"), events));
    REQUIRE_FALSE(lateModule.hasValue());
    CHECK(std::string(lateModule.error().code.value()) == "Kernel.AppKernelNotConfiguring");
}

TEST_CASE("ModuleRegistrar matches every declared contribution kind", "[kernel][modules][registrar]")
{
    AppKernel kernel;
    REQUIRE(kernel.executionServices()
                .configure(
                    std::make_shared<PassSchemaValidator>(),
                    std::make_shared<NullLogService>())
                .hasValue());
    REQUIRE(kernel.configureTaskExecutor(
        std::make_unique<InlineTaskExecutor>()).hasValue());
    REQUIRE(kernel.addModule(std::make_unique<RegistrarModule>(
        governedDescriptor("module.governed"),
        registerGovernedContributions)).hasValue());

    auto started = kernel.bootstrap();
    const auto startCode = started.hasValue()
        ? std::string {}
        : std::string(started.error().code.value());
    INFO(startCode);
    REQUIRE(started.hasValue());
    CHECK(kernel.services().contains(makeServiceId("service.module.governed")));
    CHECK(kernel.commandRegistry().descriptor(CommandKey {
        makeId<CommandName>("command.module.governed"), Version {1U, 0U, 0U}}).hasValue());
    CHECK(kernel.queryRegistry().descriptor(QueryKey {
        makeId<QueryName>("query.module.governed"), Version {1U, 0U, 0U}}).hasValue());
    CHECK(kernel.taskRegistry().descriptor(
        makeId<TaskName>("task.module.governed")).hasValue());
    CHECK(kernel.workflowRegistry().descriptor(
        makeId<WorkflowName>("workflow.module.governed")).hasValue());
    CHECK(kernel.scriptRegistry().descriptor(
        makeId<ScriptName>("script.module.governed")).hasValue());
    const auto catalog = kernel.execution().catalog();
    CHECK(catalog.modules.size() == 1U);
    CHECK(catalog.queries.size() == 1U);
    CHECK(catalog.tasks.size() == 1U);
    CHECK(catalog.workflows.size() == 1U);
    CHECK(catalog.scripts.size() == 1U);
    CHECK(std::ranges::any_of(
        catalog.commands,
        [](const CommandDescriptor& descriptor) {
            return descriptor.name == makeId<CommandName>("command.module.governed");
        }));
    REQUIRE(kernel.shutdown().hasValue());
}

TEST_CASE("ModuleRegistrar rejects undeclared and missing contributions", "[kernel][modules][registrar]")
{
    SECTION("a declared command name does not authorize another version")
    {
        AppKernel kernel;
        auto descriptor = makeDescriptor("module.undeclared-version");
        descriptor.commands = {CommandKey {
            makeId<CommandName>("command.module.versioned"), Version {1U, 0U, 0U}}};
        auto registration = [](ModuleRegistrar& registrar) {
            auto command = governedCommand("command.module.versioned");
            command.version = Version {1U, 1U, 0U};
            return registrar.registerReadOnlyCommand(
                std::move(command), std::make_shared<NullReadOnlyCommandHandler>());
        };
        REQUIRE(kernel.addModule(std::make_unique<RegistrarModule>(
            std::move(descriptor), registration)).hasValue());
        auto started = kernel.bootstrap();
        REQUIRE_FALSE(started.hasValue());
        CHECK(std::string(started.error().code.value()) == "Kernel.ModuleLifecycleFailed");
        REQUIRE(started.error().cause != nullptr);
        CHECK(std::string(started.error().cause->code.value())
              == "Kernel.ModuleContributionUndeclared");
        CHECK(kernel.commandRegistry().size() == 0U);
    }

    SECTION("undeclared registration is remembered even when the module ignores it")
    {
        AppKernel kernel;
        auto descriptor = makeDescriptor("module.undeclared");
        auto registration = [](ModuleRegistrar& registrar) {
            static_cast<void>(registrar.registerReadOnlyCommand(
                governedCommand("command.module.undeclared"),
                std::make_shared<NullReadOnlyCommandHandler>()));
            return Result<void>::success();
        };
        REQUIRE(kernel.addModule(std::make_unique<RegistrarModule>(
            std::move(descriptor), registration)).hasValue());
        auto started = kernel.bootstrap();
        REQUIRE_FALSE(started.hasValue());
        CHECK(std::string(started.error().code.value())
              == "Kernel.ModuleContributionUndeclared");
        CHECK(kernel.commandRegistry().size() == 0U);
    }

    SECTION("declared contribution must actually be registered")
    {
        AppKernel kernel;
        auto descriptor = makeDescriptor("module.missing-contribution");
        descriptor.tasks = {makeId<TaskName>("task.module.missing")};
        REQUIRE(kernel.addModule(std::make_unique<RegistrarModule>(
            std::move(descriptor),
            [](ModuleRegistrar&) { return Result<void>::success(); })).hasValue());
        auto started = kernel.bootstrap();
        REQUIRE_FALSE(started.hasValue());
        CHECK(std::string(started.error().code.value())
              == "Kernel.ModuleContributionDeclarationMismatch");
        CHECK(kernel.taskRegistry().size() == 0U);
    }
}

TEST_CASE("ModuleRuntime rolls back every registrar contribution", "[kernel][modules][registrar][rollback]")
{
    AppKernel kernel;
    REQUIRE(kernel.executionServices()
                .configure(
                    std::make_shared<PassSchemaValidator>(),
                    std::make_shared<NullLogService>())
                .hasValue());
    const auto providerId = makeModuleId("module.governed-provider");
    REQUIRE(kernel.addModule(std::make_unique<RegistrarModule>(
        governedDescriptor("module.governed-provider"),
        registerGovernedContributions)).hasValue());
    REQUIRE(kernel.addModule(std::make_unique<RegistrarModule>(
        makeDescriptor(
            "module.governed-failure",
            Version {1U, 0U, 0U},
            {{providerId, Version {1U, 0U, 0U}}}),
        [](ModuleRegistrar&) { return Result<void>::success(); },
        true)).hasValue());

    auto started = kernel.bootstrap();
    REQUIRE_FALSE(started.hasValue());
    CHECK(std::string(started.error().code.value()) == "Kernel.ModuleLifecycleFailed");
    CHECK_FALSE(kernel.services().contains(makeServiceId("service.module.governed")));
    CHECK(kernel.commandRegistry().size() == 0U);
    CHECK(kernel.queryRegistry().size() == 0U);
    CHECK(kernel.taskRegistry().size() == 0U);
    CHECK(kernel.workflowRegistry().size() == 0U);
    CHECK(kernel.scriptRegistry().size() == 0U);
}

TEST_CASE("ModuleRuntime rejects cross-module contribution ownership before callbacks", "[kernel][modules][registrar]")
{
    AppKernel kernel;
    auto first = makeDescriptor("module.owner-first");
    auto second = makeDescriptor("module.owner-second");
    const auto shared = CommandKey {
        makeId<CommandName>("command.module.shared"), Version {1U, 0U, 0U}};
    first.commands = {shared};
    second.commands = {shared};
    auto callbackInvoked = std::make_shared<bool>(false);
    auto registration = [callbackInvoked](ModuleRegistrar&) {
        *callbackInvoked = true;
        return Result<void>::success();
    };
    REQUIRE(kernel.addModule(std::make_unique<RegistrarModule>(
        std::move(first), registration)).hasValue());
    REQUIRE(kernel.addModule(std::make_unique<RegistrarModule>(
        std::move(second), registration)).hasValue());

    auto started = kernel.bootstrap();
    REQUIRE_FALSE(started.hasValue());
    CHECK(std::string(started.error().code.value())
          == "Kernel.ModuleContributionOwnerConflict");
    CHECK_FALSE(*callbackInvoked);
}
