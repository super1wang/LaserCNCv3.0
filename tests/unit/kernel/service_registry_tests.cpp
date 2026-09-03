#include <lasercnc/kernel/service_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>
#include <string>

using namespace lasercnc::kernel;

namespace {

struct ICounter {
    virtual ~ICounter() = default;
    [[nodiscard]] virtual int value() const = 0;
};

class Counter final : public ICounter {
public:
    explicit Counter(int value)
        : value_(value)
    {
    }

    [[nodiscard]] int value() const override
    {
        return value_;
    }

private:
    int value_;
};

struct IWrongService {
    virtual ~IWrongService() = default;
};

ServiceId makeServiceId(const char* value)
{
    auto result = ServiceId::create(value);
    if(!result) {
        throw std::logic_error("Invalid test service ID");
    }
    return std::move(result).value();
}

} // namespace

TEST_CASE("ServiceRegistry provides typed lookup and stable snapshots", "[kernel][services]")
{
    ServiceRegistry registry;
    const auto id = makeServiceId("service.test.counter");
    std::shared_ptr<ICounter> counter = std::make_shared<Counter>(42);

    REQUIRE(registry.registerService(id, counter).hasValue());
    REQUIRE(registry.contains(id));

    auto resolved = registry.resolve<ICounter>(id);
    REQUIRE(resolved.hasValue());
    CHECK(resolved.value()->value() == 42);

    const auto snapshot = registry.snapshot();
    REQUIRE(snapshot.serviceIds.size() == 1);
    CHECK(snapshot.serviceIds.front() == id);
    CHECK_FALSE(snapshot.frozen);
}

TEST_CASE("ServiceRegistry rejects invalid and ambiguous registrations", "[kernel][services]")
{
    ServiceRegistry registry;
    const auto id = makeServiceId("service.test.counter");
    std::shared_ptr<ICounter> counter = std::make_shared<Counter>(7);

    std::shared_ptr<ICounter> nullCounter;
    auto nullResult = registry.registerService(id, nullCounter);
    REQUIRE_FALSE(nullResult.hasValue());
    CHECK(std::string(nullResult.error().code.value()) == "Kernel.ServiceNull");

    REQUIRE(registry.registerService(id, counter).hasValue());
    auto duplicate = registry.registerService(id, counter);
    REQUIRE_FALSE(duplicate.hasValue());
    CHECK(std::string(duplicate.error().code.value()) == "Kernel.ServiceAlreadyRegistered");

    auto wrongType = registry.resolve<IWrongService>(id);
    REQUIRE_FALSE(wrongType.hasValue());
    CHECK(std::string(wrongType.error().code.value()) == "Kernel.ServiceTypeMismatch");

    const auto missingId = makeServiceId("service.test.missing");
    auto missing = registry.resolve<ICounter>(missingId);
    REQUIRE_FALSE(missing.hasValue());
    CHECK(std::string(missing.error().code.value()) == "Kernel.ServiceNotFound");
}

TEST_CASE("ServiceRegistry freeze makes composition immutable", "[kernel][services]")
{
    ServiceRegistry registry;
    registry.freeze();

    const auto id = makeServiceId("service.test.counter");
    std::shared_ptr<ICounter> counter = std::make_shared<Counter>(1);
    auto result = registry.registerService(id, counter);

    REQUIRE_FALSE(result.hasValue());
    CHECK(std::string(result.error().code.value()) == "Kernel.ServiceRegistryFrozen");
    CHECK(registry.snapshot().frozen);
}
