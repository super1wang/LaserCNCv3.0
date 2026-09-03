#include <lasercnc/state/object_type_registry.hpp>
#include <lasercnc/foundation/error.hpp>
#include <lasercnc/runtime/transaction_manager.hpp>

#include <catch2/catch_test_macros.hpp>
#include "kernel_test_module.hpp"

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace lasercnc::foundation;
using namespace lasercnc::kernel;
using namespace lasercnc::state;

static_assert(std::is_same_v<
              decltype(std::declval<lasercnc::kernel::AppKernel&>().objectTypes()),
              const ObjectTypeRegistry&>);

namespace {

template <typename Id>
Id id(const char* text)
{
    auto result = Id::create(text);
    if(!result) {
        throw std::logic_error("Invalid object type test identity");
    }
    return std::move(result).value();
}

class Validator final : public IObjectTypeValidator {
public:
    using Function = std::function<Result<void>(const Value&)>;
    explicit Validator(Function function) : function_(std::move(function)) {}
    Result<void> validate(const Value& data) const override { return function_(data); }
private:
    Function function_;
};

class References final : public IObjectReferenceEnumerator {
public:
    using Function = std::function<Result<std::vector<ObjectId>>(const Value&)>;
    explicit References(Function function) : function_(std::move(function)) {}
    Result<std::vector<ObjectId>> enumerate(const Value& data) const override
    {
        return function_(data);
    }
private:
    Function function_;
};

class Migration final : public IObjectMigration {
public:
    using Function = std::function<Result<Value>(const Value&)>;
    explicit Migration(Function function) : function_(std::move(function)) {}
    Result<Value> migrate(const Value& data) const override { return function_(data); }
private:
    Function function_;
};

std::shared_ptr<const Validator> numberValidator(std::int64_t expected)
{
    return std::make_shared<Validator>([expected](const Value& data) {
        if(data == Value {expected}) {
            return Result<void>::success();
        }
        return Result<void>::failure(makeError(
            "Test.ObjectDataInvalid", ErrorCategory::Validation,
            "Unexpected versioned object data"));
    });
}

std::shared_ptr<const References> noReferences()
{
    return std::make_shared<References>([](const Value&) {
        return Result<std::vector<ObjectId>>::success({});
    });
}

std::shared_ptr<const Migration> incrementMigration()
{
    return std::make_shared<Migration>([](const Value& data) {
        return Result<Value>::success(Value {*data.getIf<std::int64_t>() + 1});
    });
}

ObjectTypeDefinition definition(const char* name = "type.test.versioned")
{
    return ObjectTypeDefinition {
        ObjectTypeDescriptor {
            id<ObjectTypeId>(name), Version {3U, 0U, 0U}, ObjectPersistencePolicy::Durable},
        {
            {Version {3U, 0U, 0U}, numberValidator(3), noReferences()},
            {Version {1U, 0U, 0U}, numberValidator(1), noReferences()},
            {Version {2U, 0U, 0U}, numberValidator(2), noReferences()},
        },
        {
            {Version {2U, 0U, 0U}, Version {3U, 0U, 0U}, incrementMigration()},
            {Version {1U, 0U, 0U}, Version {2U, 0U, 0U}, incrementMigration()},
        }};
}

class TypeModule final : public IModule {
public:
    TypeModule(const char* moduleId, bool declareType, bool publishType, bool failStart)
        : descriptor_ {id<ModuleId>(moduleId), moduleId, Version {1U, 0U, 0U}},
          publishType_(publishType), failStart_(failStart)
    {
        if(declareType) {
            descriptor_.objectTypes = {id<ObjectTypeId>("type.test.versioned")};
        }
    }
    const ModuleDescriptor& descriptor() const noexcept override { return descriptor_; }
    Result<void> registerComponents(ModuleRegistrar& registrar) override
    {
        return publishType_ ? registrar.registerObjectType(definition())
                            : Result<void>::success();
    }
    Result<void> start(AppKernel&) override
    {
        return failStart_
            ? Result<void>::failure(makeError("Test.TypeModuleStartFailed", ErrorCategory::Internal, "failure"))
            : Result<void>::success();
    }
private:
    ModuleDescriptor descriptor_;
    bool publishType_;
    bool failStart_;
};

} // namespace

TEST_CASE("Object type admission checks the final transaction graph before publishing", "[state][object-type][admission]")
{
    using namespace lasercnc::runtime;
    ObjectTypeRegistry types;
    DocumentStore documents;
    const auto project = id<ProjectId>("project.admission");
    const auto document = id<DocumentId>("document.admission");
    const auto target = id<ObjectId>("object.target");
    const auto source = id<ObjectId>("object.source");
    auto graph = lasercnc::test::valueObjectType("type.graph");
    graph.versions.front().validator = std::make_shared<Validator>([&](const Value&) {
        // Reentrant reads prove validation is outside the DocumentStore write lock.
        // 中文翻译：重入读取证明校验发生在 DocumentStore 写锁之外。
        auto snapshot = documents.snapshot(document);
        return snapshot ? Result<void>::success() : Result<void>::failure(snapshot.error());
    });
    graph.versions.front().references = std::make_shared<References>([&](const Value& data) {
        return Result<std::vector<ObjectId>>::success(
            data == Value {"source"} ? std::vector<ObjectId>{target} : std::vector<ObjectId>{});
    });
    REQUIRE(types.registerType(std::move(graph)).hasValue());
    types.freeze();
    REQUIRE(documents.addDocument(project, document).hasValue());
    TransactionManager transactions(documents, nullptr, nullptr, nullptr, &types);
    auto seed = transactions.begin(id<TransactionId>("tx.graph.seed"), document);
    REQUIRE(seed.hasValue());
    REQUIRE(seed.value()->createObject(ObjectRecord{source, id<ObjectTypeId>("type.graph"), Value {"source"}}).hasValue());
    REQUIRE(seed.value()->createObject(ObjectRecord{target, id<ObjectTypeId>("type.graph"), Value {"target"}}).hasValue());
    REQUIRE(seed.value()->commit().hasValue());
    const auto before = documents.snapshot(document).value();
    auto edit = transactions.begin(id<TransactionId>("tx.graph.edit"), document);
    REQUIRE(edit.hasValue());
    REQUIRE(edit.value()->removeObject(target).hasValue());
    SECTION("dangling target deletion fails atomically") {
        auto committed = edit.value()->commit();
        REQUIRE_FALSE(committed.hasValue());
        CHECK(std::string(committed.error().code.value()) == "ObjectType.DanglingReference");
        CHECK(documents.snapshot(document).value().objects().all() == before.objects().all());
        CHECK(documents.snapshot(document).value().revisions() == before.revisions());
        CHECK(transactions.activeTransactionCount() == 0U);
    }
    SECTION("removing the whole graph is valid") {
        REQUIRE(edit.value()->removeObject(source).hasValue());
        REQUIRE(edit.value()->commit().hasValue());
        CHECK(documents.snapshot(document).value().objects().empty());
    }
}

TEST_CASE("Object type admission rejects unknown or invalid transaction state", "[state][object-type][admission]")
{
    using namespace lasercnc::runtime;
    ObjectTypeRegistry types;
    REQUIRE(types.registerType(definition()).hasValue());
    types.freeze();
    DocumentStore documents;
    const auto document = id<DocumentId>("document.invalid");
    REQUIRE(documents.addDocument(id<ProjectId>("project.invalid"), document).hasValue());
    TransactionManager transactions(documents, nullptr, nullptr, nullptr, &types);
    ObjectRecord object {id<ObjectId>("object.invalid"), id<ObjectTypeId>("type.test.versioned"), Value {std::int64_t {1}}};
    SECTION("unknown type") { object.type = id<ObjectTypeId>("type.unknown"); }
    SECTION("unknown version") { object.schemaVersion = Version {4U, 0U, 0U}; }
    SECTION("data does not match exact version") { object.data = Value {std::int64_t {2}}; }
    auto transaction = transactions.begin(id<TransactionId>("tx.invalid"), document);
    REQUIRE(transaction.hasValue());
    REQUIRE(transaction.value()->createObject(object).hasValue());
    CHECK_FALSE(transaction.value()->commit().hasValue());
    CHECK(documents.snapshot(document).value().objects().empty());
    CHECK(documents.snapshot(document).value().revisions() == RevisionSet{});
}

TEST_CASE("Document attach validates types before lifecycle mutation without implicit migration", "[state][object-type][admission]")
{
    AppKernel kernel;
    REQUIRE(lasercnc::test::registerObjectType(kernel, definition()).hasValue());
    REQUIRE(kernel.bootstrap().hasValue());
    const auto document = id<DocumentId>("document.attach");
    DocumentImage image {id<ProjectId>("project.attach"), document, RevisionSet{},
        {{id<ObjectId>("object.attach"), id<ObjectTypeId>("type.test.versioned"), Value {std::int64_t {1}}}}};
    bool valid = false;
    SECTION("known old schema is preserved") { valid = true; }
    SECTION("unknown type") { image.objects.front().type = id<ObjectTypeId>("type.unknown"); }
    SECTION("unknown schema") { image.objects.front().schemaVersion = Version {9U, 0U, 0U}; }
    SECTION("invalid value") { image.objects.front().data = Value {"invalid"}; }
    SECTION("duplicate stable identity") { image.objects.push_back(image.objects.front()); }
    auto attached = kernel.documentRuntime().attach(image);
    if(valid) {
        REQUIRE(attached.hasValue());
        CHECK(kernel.documents().snapshot(document).value().objects().find(image.objects.front().id)->schemaVersion == Version {1U, 0U, 0U});
    } else {
        REQUIRE_FALSE(attached.hasValue());
        CHECK_FALSE(kernel.documents().contains(document));
        CHECK(kernel.documentRuntime().list().empty());
    }
    REQUIRE(kernel.shutdown().hasValue());
}

TEST_CASE("Object persistence policy denies transient state in durable images", "[state][object-type][admission]")
{
    ObjectTypeRegistry types;
    auto transient = definition();
    transient.descriptor.persistencePolicy = ObjectPersistencePolicy::Transient;
    REQUIRE(types.registerType(std::move(transient)).hasValue());
    const std::vector records {ObjectRecord{
        id<ObjectId>("object.transient"), id<ObjectTypeId>("type.test.versioned"), Value {std::int64_t {1}}}};
    CHECK(types.validateObjects(records, false).hasValue());
    auto durable = types.validateObjects(records, true);
    REQUIRE_FALSE(durable.hasValue());
    CHECK(std::string(durable.error().code.value()) == "ObjectType.TransientPersistenceDenied");
}

TEST_CASE("Object migration is explicit transactional and rollback safe", "[state][object-type][transaction]")
{
    using namespace lasercnc::runtime;
    ObjectTypeRegistry types;
    REQUIRE(types.registerType(definition()).hasValue());
    types.freeze();
    DocumentStore documents;
    const auto project = id<ProjectId>("project.migration");
    const auto document = id<DocumentId>("document.migration");
    const auto object = id<ObjectId>("object.migration");
    REQUIRE(documents.addDocument(project, document).hasValue());
    TransactionManager transactions(documents, nullptr, nullptr, nullptr, &types);
    auto seed = transactions.begin(id<TransactionId>("tx.seed"), document);
    REQUIRE(seed.hasValue());
    REQUIRE(seed.value()->createObject(ObjectRecord{
        object, id<ObjectTypeId>("type.test.versioned"), Value {std::int64_t {1}}}).hasValue());
    REQUIRE(seed.value()->commit().hasValue());
    const auto before = documents.snapshot(document).value();
    auto transaction = transactions.begin(id<TransactionId>("tx.migrate"), document);
    REQUIRE(transaction.hasValue());
    REQUIRE(transaction.value()->migrateObject(object, Version {3U, 0U, 0U}).hasValue());
    CHECK(transaction.value()->stagedObjects().find(object)->schemaVersion == Version {3U, 0U, 0U});
    CHECK(documents.snapshot(document).value().objects().find(object)->schemaVersion == Version {1U, 0U, 0U});
    SECTION("commit preserves before and after versions") {
        auto committed = transaction.value()->commit();
        REQUIRE(committed.hasValue());
        REQUIRE(committed.value().changes.size() == 1U);
        CHECK(committed.value().changes.front().before->schemaVersion == Version {1U, 0U, 0U});
        CHECK(committed.value().changes.front().after->schemaVersion == Version {3U, 0U, 0U});
        CHECK(documents.snapshot(document).value().objects().find(object)->data == Value {std::int64_t {3}});
    }
    SECTION("explicit rollback leaves state unchanged") {
        REQUIRE(transaction.value()->rollback().hasValue());
        CHECK(documents.snapshot(document).value().objects().all() == before.objects().all());
        CHECK(documents.snapshot(document).value().revisions() == before.revisions());
    }
    SECTION("failed downgrade poisons the transaction") {
        CHECK_FALSE(transaction.value()->migrateObject(object, Version {1U, 0U, 0U}).hasValue());
        CHECK(transaction.value()->transactionState() == TransactionState::Failed);
        CHECK_FALSE(transaction.value()->commit().hasValue());
        CHECK(documents.snapshot(document).value().objects().all() == before.objects().all());
    }
    SECTION("missing object poisons the transaction") {
        CHECK_FALSE(transaction.value()->migrateObject(id<ObjectId>("object.missing"), Version {3U, 0U, 0U}).hasValue());
        CHECK_FALSE(transaction.value()->commit().hasValue());
        CHECK(documents.snapshot(document).value().revisions() == before.revisions());
    }
}

TEST_CASE("ObjectTypeRegistry freezes deterministic versioned contracts", "[state][object-type]")
{
    ObjectTypeRegistry registry;
    const auto type = id<ObjectTypeId>("type.test.versioned");
    REQUIRE(registry.registerType(definition()).hasValue());
    CHECK(registry.size() == 1U);
    REQUIRE(registry.descriptor(type).hasValue());
    CHECK(registry.descriptor(type).value().currentVersion == Version {3U, 0U, 0U});
    CHECK(registry.descriptor(type).value().persistencePolicy == ObjectPersistencePolicy::Durable);
    auto versions = registry.versions(type);
    REQUIRE(versions.hasValue());
    CHECK(versions.value() == std::vector<Version> {
        Version {1U, 0U, 0U}, Version {2U, 0U, 0U}, Version {3U, 0U, 0U}});
    CHECK_FALSE(registry.registerType(definition()).hasValue());
    CHECK_FALSE(registry.descriptor(id<ObjectTypeId>("type.unknown")).hasValue());
    CHECK_FALSE(registry.validate(type, Version {4U, 0U, 0U}, Value {std::int64_t {4}}).hasValue());
    CHECK(registry.validate(type, Version {1U, 0U, 0U}, Value {std::int64_t {1}}).hasValue());
    CHECK_FALSE(registry.validate(type, Version {3U, 0U, 0U}, Value {std::int64_t {1}}).hasValue());
    registry.freeze();
    CHECK(registry.frozen());
    auto late = registry.registerType(definition("type.late"));
    REQUIRE_FALSE(late.hasValue());
    CHECK(std::string(late.error().code.value()) == "ObjectType.RegistryFrozen");
}

TEST_CASE("ObjectTypeRegistry rejects ambiguous and incomplete migration contracts", "[state][object-type][migration]")
{
    auto candidate = definition();
    SECTION("current version missing") { candidate.versions.erase(candidate.versions.begin()); }
    SECTION("validator missing") { candidate.versions.front().validator.reset(); }
    SECTION("reference enumerator missing") { candidate.versions.front().references.reset(); }
    SECTION("duplicate version") { candidate.versions.push_back(candidate.versions.front()); }
    SECTION("future version") { candidate.versions.front().version = Version {4U, 0U, 0U}; }
    SECTION("unknown persistence policy") {
        candidate.descriptor.persistencePolicy = static_cast<ObjectPersistencePolicy>(99U);
    }
    SECTION("migration path missing") { candidate.migrations.pop_back(); }
    SECTION("migration callback missing") { candidate.migrations.front().migration.reset(); }
    SECTION("backward edge") {
        candidate.migrations.front().from = Version {3U, 0U, 0U};
        candidate.migrations.front().to = Version {2U, 0U, 0U};
    }
    SECTION("unknown target") { candidate.migrations.front().to = Version {4U, 0U, 0U}; }
    SECTION("ambiguous outgoing edge") { candidate.migrations.push_back(candidate.migrations.front()); }
    ObjectTypeRegistry registry;
    CHECK_FALSE(registry.registerType(std::move(candidate)).hasValue());
    CHECK(registry.size() == 0U);
}

TEST_CASE("ObjectTypeRegistry migrates explicitly and leaves source values unchanged", "[state][object-type][migration]")
{
    ObjectTypeRegistry registry;
    REQUIRE(registry.registerType(definition()).hasValue());
    registry.freeze();
    const auto type = id<ObjectTypeId>("type.test.versioned");
    const Value original {std::int64_t {1}};
    auto migrated = registry.migrate(type, Version {1U, 0U, 0U}, Version {3U, 0U, 0U}, original);
    REQUIRE(migrated.hasValue());
    CHECK(migrated.value() == Value {std::int64_t {3}});
    CHECK(original == Value {std::int64_t {1}});
    auto unchanged = registry.migrate(type, Version {1U, 0U, 0U}, Version {1U, 0U, 0U}, original);
    REQUIRE(unchanged.hasValue());
    CHECK(unchanged.value() == original);
    CHECK_FALSE(registry.migrate(type, Version {3U, 0U, 0U}, Version {1U, 0U, 0U},
                                Value {std::int64_t {3}}).hasValue());
    CHECK_FALSE(registry.migrate(type, Version {1U, 0U, 0U}, Version {4U, 0U, 0U}, original).hasValue());
    CHECK_FALSE(registry.migrate(type, Version {1U, 0U, 0U}, Version {3U, 0U, 0U},
                                Value {"invalid"}).hasValue());

    ObjectTypeRegistry skippedRegistry;
    auto skipped = definition();
    std::size_t migrationCalls = 0U;
    skipped.migrations.back().to = Version {3U, 0U, 0U};
    skipped.migrations.back().migration = std::make_shared<Migration>([&](const Value&) {
        ++migrationCalls;
        return Result<Value>::success(Value {std::int64_t {3}});
    });
    REQUIRE(skippedRegistry.registerType(std::move(skipped)).hasValue());
    auto unreachable = skippedRegistry.migrate(
        type, Version {1U, 0U, 0U}, Version {2U, 0U, 0U}, original);
    REQUIRE_FALSE(unreachable.hasValue());
    CHECK(std::string(unreachable.error().code.value()) == "ObjectType.MigrationPathMissing");
    CHECK(migrationCalls == 0U);
}

TEST_CASE("ObjectTypeRegistry contains callback failures without partial migration", "[state][object-type][failure]")
{
    auto candidate = definition();
    std::string expected;
    SECTION("migration returns failure") {
        expected = "ObjectType.MigrationFailed";
        candidate.migrations.back().migration = std::make_shared<Migration>([](const Value&) {
            return Result<Value>::failure(makeError("Test.MigrationFailed", ErrorCategory::Validation, "failure"));
        });
    }
    SECTION("migration throws") {
        expected = "ObjectType.MigrationException";
        candidate.migrations.back().migration = std::make_shared<Migration>([](const Value&) -> Result<Value> {
            throw std::runtime_error("migration failure");
        });
    }
    SECTION("intermediate result violates next schema") {
        expected = "ObjectType.ValidationFailed";
        candidate.migrations.back().migration = std::make_shared<Migration>([](const Value&) {
            return Result<Value>::success(Value {std::int64_t {99}});
        });
    }
    SECTION("source validator throws") {
        expected = "ObjectType.ValidatorException";
        candidate.versions[1U].validator = std::make_shared<Validator>([](const Value&) -> Result<void> {
            throw std::runtime_error("validator failure");
        });
    }
    ObjectTypeRegistry registry;
    REQUIRE(registry.registerType(std::move(candidate)).hasValue());
    const Value original {std::int64_t {1}};
    auto migrated = registry.migrate(id<ObjectTypeId>("type.test.versioned"),
                                    Version {1U, 0U, 0U}, Version {3U, 0U, 0U}, original);
    REQUIRE_FALSE(migrated.hasValue());
    CHECK(std::string(migrated.error().code.value()) == expected);
    CHECK(original == Value {std::int64_t {1}});
}

TEST_CASE("ObjectTypeRegistry normalizes references and runs callbacks outside locks", "[state][object-type][references][concurrency]")
{
    ObjectTypeRegistry registry;
    auto candidate = definition();
    std::atomic_size_t calls {0U};
    std::atomic_bool callbacksValid {true};
    candidate.versions[1U].validator = std::make_shared<Validator>([&](const Value&) {
        if(!registry.frozen() || registry.descriptors().size() != 1U) {
            callbacksValid.store(false);
        }
        return Result<void>::success();
    });
    candidate.versions[1U].references = std::make_shared<References>([&](const Value&) {
        if(registry.size() != 1U) {
            callbacksValid.store(false);
        }
        calls.fetch_add(1U);
        return Result<std::vector<ObjectId>>::success({
            id<ObjectId>("object.z"), id<ObjectId>("object.a"), id<ObjectId>("object.z")});
    });
    REQUIRE(registry.registerType(std::move(candidate)).hasValue());
    registry.freeze();
    std::vector<std::future<Result<std::vector<ObjectId>>>> futures;
    for(std::size_t index = 0U; index < 16U; ++index) {
        futures.push_back(std::async(std::launch::async, [&] {
            return registry.references(id<ObjectTypeId>("type.test.versioned"),
                                       Version {1U, 0U, 0U}, Value {std::int64_t {1}});
        }));
    }
    for(auto& future : futures) {
        auto result = future.get();
        REQUIRE(result.hasValue());
        CHECK(result.value() == std::vector<ObjectId> {id<ObjectId>("object.a"), id<ObjectId>("object.z")});
    }
    CHECK(calls.load() == 16U);
    CHECK(callbacksValid.load());
}

TEST_CASE("ObjectTypeRegistry contains reference enumeration failures", "[state][object-type][references][failure]")
{
    auto candidate = definition();
    std::string expected;
    SECTION("returned error") {
        expected = "ObjectType.ReferenceEnumerationFailed";
        candidate.versions[1U].references = std::make_shared<References>([](const Value&) {
            return Result<std::vector<ObjectId>>::failure(makeError(
                "Test.ReferencesFailed", ErrorCategory::Validation, "failure"));
        });
    }
    SECTION("exception") {
        expected = "ObjectType.ReferenceEnumerationException";
        candidate.versions[1U].references = std::make_shared<References>([](const Value&) -> Result<std::vector<ObjectId>> {
            throw std::runtime_error("references failure");
        });
    }
    ObjectTypeRegistry registry;
    REQUIRE(registry.registerType(std::move(candidate)).hasValue());
    auto references = registry.references(id<ObjectTypeId>("type.test.versioned"),
                                          Version {1U, 0U, 0U}, Value {std::int64_t {1}});
    REQUIRE_FALSE(references.hasValue());
    CHECK(std::string(references.error().code.value()) == expected);
}

TEST_CASE("AppKernel governs object types through modules and read-only discovery", "[kernel][object-type][modules]")
{
    AppKernel kernel;
    REQUIRE(lasercnc::test::registerObjectType(kernel, definition()).hasValue());
    REQUIRE(kernel.bootstrap().hasValue());
    CHECK(kernel.objectTypes().frozen());
    CHECK(kernel.objectTypes().size() == 1U);
    const auto catalog = kernel.execution().catalog();
    REQUIRE(catalog.objectTypes.size() == 1U);
    CHECK(catalog.objectTypes.front().type == id<ObjectTypeId>("type.test.versioned"));
    CHECK_FALSE(lasercnc::test::registerObjectType(kernel, definition("type.late")).hasValue());
    REQUIRE(kernel.shutdown().hasValue());
}

TEST_CASE("ModuleRegistrar rejects invalid object type ownership and rolls back", "[kernel][object-type][modules][rollback]")
{
    AppKernel kernel;
    SECTION("undeclared type") {
        REQUIRE(kernel.addModule(std::make_unique<TypeModule>(
            "module.type", false, true, false)).hasValue());
    }
    SECTION("declared but not published") {
        REQUIRE(kernel.addModule(std::make_unique<TypeModule>(
            "module.type", true, false, false)).hasValue());
    }
    SECTION("start failure removes the published type") {
        REQUIRE(kernel.addModule(std::make_unique<TypeModule>(
            "module.type", true, true, true)).hasValue());
    }
    SECTION("cross-module ownership conflict") {
        REQUIRE(kernel.addModule(std::make_unique<TypeModule>(
            "module.type.first", true, true, false)).hasValue());
        REQUIRE(kernel.addModule(std::make_unique<TypeModule>(
            "module.type.second", true, true, false)).hasValue());
    }
    CHECK_FALSE(kernel.bootstrap().hasValue());
    CHECK(kernel.objectTypes().size() == 0U);
    CHECK(kernel.execution().catalog().objectTypes.empty());
}
