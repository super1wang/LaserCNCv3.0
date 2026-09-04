#include <lasercnc/infrastructure/filesystem_asset_store.hpp>
#include "snapshot_storage_fixture.hpp"
#include <lasercnc/infrastructure/filesystem_snapshot_store.hpp>
#include <lasercnc/infrastructure/jsoncons_adapter.hpp>
#include <lasercnc/infrastructure/sha256_hash_service.hpp>
#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>
#include <lasercnc/runtime/asset_validation.hpp>
#include <lasercnc/runtime/transaction_manager.hpp>

#include <catch2/catch_test_macros.hpp>
#include "kernel_test_module.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>

using namespace lasercnc::foundation;
using namespace lasercnc::kernel;
using namespace lasercnc::runtime;
using namespace lasercnc::state;
using namespace lasercnc::infrastructure;
using lasercnc::test::requiredTestId;

namespace {

class TemporaryRoot final {
public:
    explicit TemporaryRoot(bool retain = false) : retain_(retain)
    {
        static std::atomic_ullong sequence{0U};
        const auto base = retain ? std::filesystem::path{LCNC_STRESS_TEST_ROOT} / "offline-backup"
                                 : std::filesystem::temp_directory_path();
        path = base / ("lasercnc-asset-state-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
            + '-' + std::to_string(sequence.fetch_add(1U)));
    }
    ~TemporaryRoot()
    {
        if(retain_) { return; }
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
private:
    bool retain_;
};

std::span<const std::byte> bytes(std::string_view value)
{
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

class NullLog final : public lasercnc::observability::ILogService {
public:
    Result<void> write(const lasercnc::observability::LogRecord&) override { return Result<void>::success(); }
    Result<void> flush() override { return Result<void>::success(); }
};

class AssetHandler final : public ICommandHandler {
public:
    Result<Value> execute(const CommandRequest& request, ApplicationTransaction& transaction) override
    {
        ++calls;
        const auto object = requiredTestId<ObjectId>("object.asset");
        auto changed = request.command == requiredTestId<CommandName>("test.asset.create")
            ? transaction.createObject(ObjectRecord{object, requiredTestId<ObjectTypeId>("type.asset"),
                Value {"metadata"}, Version{1U, 0U, 0U}, assets})
            : transaction.replaceObjectAssets(object, assets);
        if(!changed) {
            return Result<Value>::failure(std::move(changed).error());
        }
        return Result<Value>::success(Value {"done"});
    }
    std::vector<AssetRef> assets;
    unsigned int calls{0U};
};

std::shared_ptr<FilesystemAssetStore> createStore(const TemporaryRoot& root)
{
    auto store = FilesystemAssetStore::create({root.path / "assets", 4096U}, std::make_shared<Sha256HashService>());
    if(!store) {
        throw std::logic_error("Cannot initialize asset test storage");
    }
    return std::move(store).value();
}

AssetRef publish(FilesystemAssetStore& store, const char* content)
{
    auto result = store.publish(requiredTestId<AssetKind>("test.binary"), bytes(content));
    if(!result) {
        throw std::logic_error("Cannot publish test asset");
    }
    return std::move(result).value();
}

const auto project = requiredTestId<ProjectId>("project.asset");
const auto document = requiredTestId<DocumentId>("document.asset");
const auto session = requiredTestId<SessionId>("session.asset");

CommandRequest request(const char* id, const char* name = "test.asset.create", bool idempotent = false)
{
    CommandRequest result{requiredTestId<RequestId>(id), {session, project, document},
        requiredTestId<CommandName>(name), Version{1U, 0U, 0U}, Value{Value::Object{}}, std::nullopt,
        requiredTestId<CorrelationId>("correlation.asset"), requiredTestId<TraceId>("trace.asset")};
    if(idempotent) {
        result.idempotencyKey = requiredTestId<IdempotencyKey>("key.asset.create");
    }
    return result;
}

void configure(AppKernel& kernel, const TemporaryRoot& root,
               std::shared_ptr<lasercnc::platform::IAssetStore> store,
               const std::shared_ptr<AssetHandler>& handler, bool addDocument)
{
    if(store != nullptr) {
        REQUIRE(kernel.configureAssetStore(std::move(store)).hasValue());
    }
    REQUIRE(kernel.executionServices().configure(std::make_shared<JsonconsAdapter>(), std::make_shared<NullLog>()).hasValue());
    REQUIRE(lasercnc::test::registerObjectType(kernel, lasercnc::test::valueObjectType("type.asset")).hasValue());
    for(const auto* name : {"test.asset.create", "test.asset.replace"}) {
        CommandDescriptor descriptor{requiredTestId<CommandName>(name), Version{1U, 0U, 0U},
            lasercnc::test::testAnySchema("schema.asset.input"), lasercnc::test::testAnySchema("schema.asset.output"),
            ExecutionMode::Synchronous, SideEffectLevel::DocumentWrite,
            requiredTestId<CapabilityId>("document.write"), true, true, true};
        REQUIRE(lasercnc::test::registerCommand(kernel, std::move(descriptor), handler).hasValue());
    }
    REQUIRE(kernel.capabilities().replace(session, std::array{requiredTestId<CapabilityId>("document.write"),
        requiredTestId<CapabilityId>("kernel.history.edit")}).hasValue());
    auto backend = SqlitePersistenceBackend::open({root.path / "state.db"});
    REQUIRE(backend.hasValue());
    auto snapshots = FilesystemSnapshotStore::create({root.path / "snapshots", 1024U * 1024U});
    REQUIRE(snapshots.hasValue());
    REQUIRE(kernel.configurePersistence(std::move(backend).value(), std::make_shared<JsonconsAdapter>(),
        std::make_shared<Sha256HashService>(), std::move(snapshots).value()).hasValue());
    if(addDocument) {
        REQUIRE(kernel.addDocument(project, document).hasValue());
    }
}

std::vector<AssetRef> objectAssets(const AppKernel& kernel)
{
    return kernel.documents().snapshot(document).value().objects().find(requiredTestId<ObjectId>("object.asset"))->assets;
}

void damageAsset(const TemporaryRoot& root, const AssetRef& reference)
{
    std::ofstream file(lasercnc::test::snapshotStoragePath(root.path / "assets", reference.id.value()),
        std::ios::binary | std::ios::trunc);
    file << "corrupt";
    REQUIRE(file.good());
}

std::map<std::string, std::string> bundleDigests(const TemporaryRoot& root)
{
    std::map<std::string, std::string> result;
    for(const auto& entry : std::filesystem::recursive_directory_iterator(root.path)) {
        REQUIRE_FALSE(entry.is_symlink());
        if(!entry.is_regular_file()) { continue; }
        std::ifstream input(entry.path(), std::ios::binary);
        REQUIRE(input.good());
        const std::string content{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        REQUIRE_FALSE(input.bad());
        const auto digest = Sha256HashService{}.digest(bytes(content));
        REQUIRE(digest);
        result.emplace(std::filesystem::relative(entry.path(), root.path).generic_string(),
            std::string(digest.value().value()));
    }
    return result;
}

void copyClosedBundle(const TemporaryRoot& source, const TemporaryRoot& destination)
{
    // Only callers that have destroyed every source Host may copy this clean fixture.
    // 中文翻译：只有已销毁所有源 Host 的调用方才能复制这个干净夹具，不能用于热备份。
    REQUIRE_FALSE(std::filesystem::exists(source.path / "state.db-journal"));
    REQUIRE_FALSE(std::filesystem::exists(source.path / "state.db-wal"));
    REQUIRE_FALSE(std::filesystem::exists(destination.path));
    std::filesystem::copy(source.path, destination.path, std::filesystem::copy_options::recursive);
    REQUIRE(bundleDigests(source) == bundleDigests(destination));
}

} // namespace

TEST_CASE("Offline storage bundles restore matching generations and reject missing referenced files", "[asset][recovery][backup][c5]")
{
    for(unsigned int variant = 0U; variant < 6U; ++variant) {
        DYNAMIC_SECTION("variant=" << variant) {
            TemporaryRoot source{true};
            TemporaryRoot older{true};
            TemporaryRoot newer{true};
            TemporaryRoot restored{true};
            INFO("source=" << source.path.string());
            INFO("older=" << older.path.string());
            INFO("newer=" << newer.path.string());
            INFO("restored=" << restored.path.string());
            std::optional<AssetRef> first;
            std::optional<AssetRef> second;
            std::optional<Document> oldImage;
            std::optional<Document> newImage;
            {
                auto store = createStore(source);
                first = publish(*store, "offline generation one");
                AppKernel kernel;
                auto handler = std::make_shared<AssetHandler>();
                handler->assets = {*first};
                configure(kernel, source, store, handler, true);
                REQUIRE(kernel.bootstrap());
                REQUIRE(kernel.execution().executeCommand(request("request.offline.seed")));
                oldImage = kernel.documents().snapshot(document).value();
                REQUIRE(kernel.documentRuntime().close(document));
                REQUIRE(kernel.documentRuntime().open(document));
                REQUIRE(kernel.shutdown());
            }
            copyClosedBundle(source, older);
            const auto oldDigests = bundleDigests(older);
            {
                auto store = createStore(source);
                second = publish(*store, "offline generation two");
                AppKernel kernel;
                auto handler = std::make_shared<AssetHandler>();
                handler->assets = {*second};
                configure(kernel, source, store, handler, false);
                REQUIRE(kernel.bootstrap());
                REQUIRE(kernel.execution().executeCommand(request("request.offline.replace", "test.asset.replace")));
                newImage = kernel.documents().snapshot(document).value();
                REQUIRE(kernel.documentRuntime().close(document));
                REQUIRE(kernel.documentRuntime().open(document));
                REQUIRE(kernel.shutdown());
            }
            copyClosedBundle(source, newer);
            const auto newDigests = bundleDigests(newer);
            REQUIRE(oldDigests != newDigests);
            const bool oldGeneration = variant == 0U || variant == 2U;
            const bool rejected = variant >= 3U;
            if(variant <= 1U) { copyClosedBundle(oldGeneration ? older : newer, restored); }
            else {
                REQUIRE(std::filesystem::create_directory(restored.path));
                const auto& databaseSource = oldGeneration ? older : newer;
                REQUIRE(std::filesystem::copy_file(databaseSource.path / "state.db", restored.path / "state.db"));
                const auto& snapshotSource = variant == 3U ? older : newer;
                const auto& assetSource = variant == 4U ? older : newer;
                std::filesystem::copy(snapshotSource.path / "snapshots", restored.path / "snapshots",
                    std::filesystem::copy_options::recursive);
                std::filesystem::copy(assetSource.path / "assets", restored.path / "assets",
                    std::filesystem::copy_options::recursive);
                if(variant == 5U) {
                    // Remove only this copied historical asset to model an incomplete archive.
                    // 中文翻译：只移除这个恢复副本中的历史资产，模拟归档遗漏；源与两份归档均保留。
                    REQUIRE(std::filesystem::remove(lasercnc::test::snapshotStoragePath(
                        restored.path / "assets", first->id.value())));
                }
            }
            const auto candidateDigests = bundleDigests(restored);
            std::optional<Document> accepted = oldGeneration ? oldImage : newImage;
            for(unsigned int restart = 0U; restart < 2U; ++restart) {
                auto store = createStore(restored);
                AppKernel kernel;
                auto handler = std::make_shared<AssetHandler>();
                configure(kernel, restored, store, handler, false);
                const auto boot = kernel.bootstrap();
                if(rejected) {
                    REQUIRE_FALSE(boot);
                    const char* outer = variant == 3U ? "Persistence.KernelRecoveryFailed"
                        : variant == 4U ? "ObjectType.RecoveryAdmissionFailed" : "ObjectType.HistoryAdmissionFailed";
                    CHECK(std::string(boot.error().code.value()) == outer);
                    REQUIRE(boot.error().cause);
                    CHECK(std::string(boot.error().cause->code.value()) ==
                        (variant == 3U ? "Snapshot.NotFound" : "Asset.StateAdmissionFailed"));
                    if(variant != 3U) {
                        const auto* detail = boot.error().cause->details.getIf<Value::Object>();
                        REQUIRE(detail);
                        CHECK(detail->at("assetId") == Value{std::string((variant == 4U ? second : first)->id.value())});
                    }
                    CHECK(kernel.state() == AppKernelState::Failed);
                    CHECK_FALSE(kernel.documents().contains(document));
                    CHECK_FALSE(kernel.documentRuntime().accepting());
                    CHECK_FALSE(kernel.execution().executeCommand(request("request.offline.denied")));
                    CHECK(handler->calls == 0U);
                } else {
                    REQUIRE(boot);
                    const auto image = kernel.documents().snapshot(document).value();
                    CHECK(image.objects().all() == accepted->objects().all());
                    CHECK(image.revisions() == accepted->revisions());
                    CHECK(handler->calls == 0U);
                    CHECK(objectAssets(kernel) == std::vector<AssetRef>{oldGeneration ? *first : *second});
                    CHECK(kernel.history().snapshot(document).value().cursor ==
                        (oldGeneration ? HistoryCursor{1U, 1U} : HistoryCursor{2U, 2U}));
                    if(restart == 0U) {
                        REQUIRE(kernel.execution().executeCommand(request("request.offline.undo", "edit.undo")));
                        if(oldGeneration) { CHECK(kernel.documents().snapshot(document).value().objects().empty()); }
                        else { CHECK(objectAssets(kernel) == std::vector<AssetRef>{*first}); }
                        REQUIRE(kernel.execution().executeCommand(request("request.offline.redo", "edit.redo")));
                        CHECK(objectAssets(kernel) == std::vector<AssetRef>{oldGeneration ? *first : *second});
                        accepted = kernel.documents().snapshot(document).value();
                    }
                    CHECK(handler->calls == 0U);
                    REQUIRE(kernel.shutdown());
                }
            }
            if(rejected) { CHECK(bundleDigests(restored) == candidateDigests); }
            CHECK(bundleDigests(older) == oldDigests);
            CHECK(bundleDigests(newer) == newDigests);
            CHECK(bundleDigests(source) == newDigests);
        }
    }
}

TEST_CASE("Asset references survive transactions snapshots idempotency and history restart", "[asset][state][recovery]")
{
    TemporaryRoot root;
    auto store = createStore(root);
    const auto first = publish(*store, "first binary");
    const auto second = publish(*store, "second binary");
    {
        AppKernel kernel;
        auto handler = std::make_shared<AssetHandler>();
        handler->assets = {first};
        configure(kernel, root, store, handler, true);
        REQUIRE(kernel.bootstrap().hasValue());
        REQUIRE(kernel.execution().executeCommand(request("request.asset.create", "test.asset.create", true)).hasValue());
        CHECK(objectAssets(kernel) == std::vector<AssetRef>{first});
        handler->assets = {second};
        auto replaced = kernel.execution().executeCommand(request("request.asset.replace", "test.asset.replace"));
        REQUIRE(replaced.hasValue());
        CHECK(replaced.value().commit->changes.front().before->assets == std::vector<AssetRef>{first});
        CHECK(replaced.value().commit->changes.front().after->assets == std::vector<AssetRef>{second});
        REQUIRE(kernel.documentRuntime().close(document).hasValue());
        REQUIRE(kernel.documentRuntime().open(document).hasValue());
        CHECK(objectAssets(kernel) == std::vector<AssetRef>{second});
        REQUIRE(kernel.shutdown().hasValue());
    }
    {
        AppKernel kernel;
        auto handler = std::make_shared<AssetHandler>();
        configure(kernel, root, createStore(root), handler, false);
        REQUIRE(kernel.bootstrap().hasValue());
        CHECK(objectAssets(kernel) == std::vector<AssetRef>{second});
        auto replay = kernel.execution().executeCommand(request("request.asset.replay", "test.asset.create", true));
        REQUIRE(replay.hasValue());
        CHECK(replay.value().replayed);
        CHECK(replay.value().commit->changes.front().after->assets == std::vector<AssetRef>{first});
        CHECK(handler->calls == 0U);
        REQUIRE(kernel.execution().executeCommand(request("request.asset.undo", "edit.undo")).hasValue());
        CHECK(objectAssets(kernel) == std::vector<AssetRef>{first});
        REQUIRE(kernel.shutdown().hasValue());
    }
    {
        AppKernel kernel;
        configure(kernel, root, createStore(root), std::make_shared<AssetHandler>(), false);
        REQUIRE(kernel.bootstrap().hasValue());
        REQUIRE(kernel.execution().executeCommand(request("request.asset.redo", "edit.redo")).hasValue());
        CHECK(objectAssets(kernel) == std::vector<AssetRef>{second});
        REQUIRE(kernel.shutdown().hasValue());
    }
}

TEST_CASE("Asset admission denies missing store forged or damaged references before commit", "[asset][state][failure]")
{
    TemporaryRoot root;
    auto store = createStore(root);
    auto reference = publish(*store, "valid asset");
    bool noStore = false;
    bool duplicate = false;
    SECTION("missing store") { noStore = true; }
    SECTION("forged metadata") { ++reference.byteSize; }
    SECTION("damaged file") { damageAsset(root, reference); }
    SECTION("duplicate identity") { duplicate = true; }
    AppKernel kernel;
    auto handler = std::make_shared<AssetHandler>();
    handler->assets = {reference};
    if(duplicate) { handler->assets.push_back(reference); }
    configure(kernel, root, noStore ? nullptr : store, handler, true);
    REQUIRE(kernel.bootstrap().hasValue());
    CHECK_FALSE(kernel.configureAssetStore(store).hasValue());
    auto rejected = kernel.execution().executeCommand(request("request.asset.invalid"));
    REQUIRE_FALSE(rejected.hasValue());
    CHECK(std::string(rejected.error().code.value()) ==
        (noStore ? "Asset.StoreRequired" : (duplicate ? "Asset.DuplicateReference" : "Asset.StateAdmissionFailed")));
    CHECK(kernel.documents().snapshot(document).value().objects().empty());
    CHECK(kernel.documents().snapshot(document).value().revisions() == RevisionSet{});
    CHECK(kernel.persistence().journalAfter(document, 0U).value().empty());
    CHECK(kernel.history().snapshot(document).value().entries.empty());
    const auto other = requiredTestId<DocumentId>("document.asset.invalid-import");
    REQUIRE(kernel.documentRuntime().create(project, other));
    auto otherRequest = request("request.asset.invalid-import");
    otherRequest.context.documentId = other;
    CHECK_FALSE(kernel.execution().executeCommand(otherRequest));
    CHECK(kernel.documents().snapshot(other).value().objects().empty());
    CHECK(kernel.documents().snapshot(other).value().revisions() == RevisionSet{});
    CHECK(kernel.persistence().journalAfter(other, 0U).value().empty());
    CHECK(kernel.history().snapshot(other).value().entries.empty());
    CHECK(kernel.persistence().documentCatalog().value().size() == 2U);
    REQUIRE(kernel.shutdown().hasValue());
}

TEST_CASE("Recovery rejects missing assets including historical references", "[asset][state][recovery]")
{
    TemporaryRoot root;
    auto store = createStore(root);
    const auto reference = publish(*store, "asset before recovery");
    bool historicalOnly = false;
    bool noStore = false;
    SECTION("current asset damaged") {}
    SECTION("asset only in history damaged") { historicalOnly = true; }
    SECTION("asset store not configured") { noStore = true; }
    {
        AppKernel kernel;
        auto handler = std::make_shared<AssetHandler>();
        handler->assets = {reference};
        configure(kernel, root, store, handler, true);
        REQUIRE(kernel.bootstrap().hasValue());
        REQUIRE(kernel.execution().executeCommand(request("request.asset.seed")).hasValue());
        if(historicalOnly) {
            handler->assets.clear();
            REQUIRE(kernel.execution().executeCommand(request("request.asset.clear", "test.asset.replace")).hasValue());
        }
        REQUIRE(kernel.shutdown().hasValue());
    }
    if(!noStore) { damageAsset(root, reference); }
    {
        AppKernel kernel;
        configure(kernel, root, noStore ? nullptr : store, std::make_shared<AssetHandler>(), false);
        auto recovered = kernel.bootstrap();
        REQUIRE_FALSE(recovered.hasValue());
        CHECK_FALSE(kernel.documents().contains(document));
        CHECK_FALSE(kernel.documentRuntime().accepting());
        CHECK(kernel.objectTypes().size() == 0U);
    }
}

TEST_CASE("Damaged assets prevent undo and reopen without changing state", "[asset][state][history]")
{
    TemporaryRoot root;
    auto store = createStore(root);
    const auto reference = publish(*store, "asset to corrupt");
    AppKernel kernel;
    auto handler = std::make_shared<AssetHandler>();
    handler->assets = {reference};
    configure(kernel, root, store, handler, true);
    REQUIRE(kernel.bootstrap().hasValue());
    REQUIRE(kernel.execution().executeCommand(request("request.asset.seed")).hasValue());
    SECTION("undo cannot restore damaged historical asset") {
        handler->assets.clear();
        REQUIRE(kernel.execution().executeCommand(request("request.asset.clear", "test.asset.replace")).hasValue());
        const auto before = kernel.documents().snapshot(document).value();
        const auto history = kernel.history().snapshot(document).value().cursor;
        damageAsset(root, reference);
        auto rejected = kernel.execution().executeCommand(request("request.asset.bad-undo", "edit.undo"));
        REQUIRE_FALSE(rejected.hasValue());
        CHECK(std::string(rejected.error().code.value()) == "Asset.StateAdmissionFailed");
        CHECK(kernel.documents().snapshot(document).value().revisions() == before.revisions());
        CHECK(kernel.history().snapshot(document).value().cursor == history);
        CHECK(objectAssets(kernel).empty());
    }
    SECTION("open remains detached on missing asset") {
        REQUIRE(kernel.documentRuntime().close(document).hasValue());
        damageAsset(root, reference);
        CHECK_FALSE(kernel.documentRuntime().open(document).hasValue());
        CHECK_FALSE(kernel.documents().contains(document));
        CHECK(kernel.documentRuntime().lifecycle(document).value().state == DocumentLifecycleState::Detached);
    }
    SECTION("clearing broken assets cannot persist dangling historical material") {
        damageAsset(root, reference);
        handler->assets.clear();
        const auto before = kernel.documents().snapshot(document).value().revisions();
        CHECK_FALSE(kernel.execution().executeCommand(request("request.asset.bad-clear", "test.asset.replace")).hasValue());
        CHECK(kernel.documents().snapshot(document).value().revisions() == before);
        CHECK(kernel.persistence().journalAfter(document, 0U).value().size() == 1U);
    }
    SECTION("close refuses a snapshot with damaged assets and retains the document") {
        damageAsset(root, reference);
        CHECK_FALSE(kernel.documentRuntime().close(document).hasValue());
        CHECK(kernel.documents().contains(document));
        CHECK(kernel.documentRuntime().lifecycle(document).value().state == DocumentLifecycleState::Failed);
    }
    REQUIRE(kernel.shutdown().hasValue());
}

TEST_CASE("Asset verification is lock free for document reads and exceptions fail closed", "[asset][state][transaction]")
{
    class CheckingStore final : public lasercnc::platform::IAssetStore {
    public:
        Result<AssetRef> publish(const AssetKind&, std::span<const std::byte>) override
        { throw std::logic_error("Unused publish"); }
        Result<std::vector<std::byte>> read(const AssetRef&) const override
        { throw std::logic_error("Unused read"); }
        Result<void> verify(const AssetRef&) const override { return check(); }
        std::function<Result<void>()> check;
    } store;
    DocumentStore documents;
    REQUIRE(documents.addDocument(project, document).hasValue());
    TransactionManager transactions(documents, nullptr, nullptr, nullptr, nullptr, &store);
    bool fail = false;
    SECTION("snapshot reads are reentrant") {}
    SECTION("store exception blocks commit") { fail = true; }
    store.check = [&] {
        if(fail) { throw std::runtime_error("Injected verification exception"); }
        CHECK(documents.snapshot(document).hasValue());
        return Result<void>::success();
    };
    auto transaction = transactions.begin(requiredTestId<TransactionId>("tx.asset"), document);
    REQUIRE(transaction.hasValue());
    const AssetRef reference{requiredTestId<AssetId>("test.asset"), requiredTestId<ContentDigest>("test.digest"),
        requiredTestId<AssetKind>("test.binary"), 4U};
    REQUIRE(transaction.value()->createObject({requiredTestId<ObjectId>("object.asset"),
        requiredTestId<ObjectTypeId>("type.asset"), Value{}, Version{1U, 0U, 0U}, {reference}}).hasValue());
    auto committed = transaction.value()->commit();
    CHECK(committed.hasValue() == !fail);
    CHECK(documents.snapshot(document).value().objects().empty() == fail);
}

TEST_CASE("Asset admission shares exact references but rejects conflicting identities", "[asset][state][validation]")
{
    class CountingStore final : public lasercnc::platform::IAssetStore {
    public:
        Result<AssetRef> publish(const AssetKind&, std::span<const std::byte>) override
        { throw std::logic_error("Unused publish"); }
        Result<std::vector<std::byte>> read(const AssetRef&) const override
        { throw std::logic_error("Unused read"); }
        Result<void> verify(const AssetRef&) const override
        { ++calls; return Result<void>::success(); }
        mutable unsigned int calls{0U};
    };
    auto store = std::make_shared<CountingStore>();
    const AssetRef reference{requiredTestId<AssetId>("test.shared"), requiredTestId<ContentDigest>("test.digest"),
        requiredTestId<AssetKind>("test.binary"), 4U};
    std::vector<ObjectRecord> objects{
        {requiredTestId<ObjectId>("object.first"), requiredTestId<ObjectTypeId>("type.asset"),
            Value{}, Version{1U, 0U, 0U}, {reference}},
        {requiredTestId<ObjectId>("object.second"), requiredTestId<ObjectTypeId>("type.asset"),
            Value{}, Version{1U, 0U, 0U}, {reference}},
    };
    REQUIRE(validateObjectAssets(objects, store.get()).hasValue());
    CHECK(store->calls == 1U);
    ++objects.back().assets.front().byteSize;
    auto conflict = validateObjectAssets(objects, store.get());
    REQUIRE_FALSE(conflict.hasValue());
    CHECK(std::string(conflict.error().code.value()) == "Asset.ReferenceConflict");
    CHECK(store->calls == 2U);
    CHECK(validateObjectAssets({}, nullptr).hasValue());
    AppKernel kernel;
    CHECK_FALSE(kernel.configureAssetStore(nullptr).hasValue());
    REQUIRE(kernel.configureAssetStore(store).hasValue());
    CHECK_FALSE(kernel.configureAssetStore(store).hasValue());
}
