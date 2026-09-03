#include <lasercnc/kernel/app_kernel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace lasercnc::foundation;
using namespace lasercnc::kernel;

namespace {

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

    [[nodiscard]] Result<void> registerServices(ServiceRegistry& services) override
    {
        append("register");
        if(serviceToRegister_.has_value() && registerDeclaredService_) {
            std::shared_ptr<IProbeService> service = std::make_shared<ProbeService>();
            return services.registerService(*serviceToRegister_, std::move(service));
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

    [[nodiscard]] Result<void> registerServices(ServiceRegistry&) override
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

} // namespace

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
    CHECK(std::string(result.error().code.value()) == "Kernel.ModuleServiceDeclarationMismatch");
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
    CHECK(std::string(result.error().code.value()) == "Kernel.ServiceProviderConflict");
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
