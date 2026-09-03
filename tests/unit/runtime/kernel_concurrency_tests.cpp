#include "kernel_test_module.hpp"

#include <lasercnc/infrastructure/filesystem_snapshot_store.hpp>
#include <lasercnc/infrastructure/jsoncons_adapter.hpp>
#include <lasercnc/infrastructure/sha256_hash_service.hpp>
#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace lasercnc;
using namespace foundation;
using namespace kernel;
using namespace runtime;
using namespace state;
using namespace infrastructure;
using namespace std::chrono_literals;

template<typename T> T take(Result<T> result)
{
    if(!result) { throw std::runtime_error(std::string(result.error().code.value()) + ": " + result.error().message); }
    return std::move(result).value();
}
void take(Result<void> result)
{
    if(!result) { throw std::runtime_error(std::string(result.error().code.value()) + ": " + result.error().message); }
}
template<typename Id> Id id(const char* value) { return take(Id::create(value)); }
const auto project = id<ProjectId>("project.stress");
const auto document = id<DocumentId>("document.stress");
const auto session = id<SessionId>("session.stress");
constexpr unsigned int rounds = 20U;

class TimedGate final {
public:
    void arriveAndWait()
    {
        std::unique_lock lock(mutex_);
        ++arrivals_;
        changed_.notify_all();
        if(!changed_.wait_for(lock, 5s, [this] { return released_; })) {
            throw std::runtime_error("Test.GateTimeout: worker release");
        }
    }
    bool awaitArrivals(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 5s, [this, count] { return arrivals_ >= count; });
    }
    void release()
    {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t arrivals_{0U};
    bool released_{false};
};

// Declare after futures: a failed assertion must release workers before joining them.
// 中文翻译：在 future 之后声明，断言失败时先释放工作线程，再等待其退出。
struct ReleaseOnExit final {
    TimedGate& gate;
    ~ReleaseOnExit() { gate.release(); }
};

template<typename T> T completed(std::future<T>& future)
{
    if(future.wait_for(5s) != std::future_status::ready) {
        throw std::runtime_error("Test.FutureTimeout: execution did not finish");
    }
    return future.get();
}
template<typename Predicate> bool awaitCondition(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        if(predicate()) { return true; }
        std::this_thread::yield();
    } while(std::chrono::steady_clock::now() < deadline);
    return false;
}

class NullLog final : public observability::ILogService {
public:
    Result<void> write(const observability::LogRecord&) override { return Result<void>::success(); }
    Result<void> flush() override { return Result<void>::success(); }
};

class CreateHandler final : public ICommandHandler {
public:
    Result<Value> execute(const CommandRequest& request, ApplicationTransaction& transaction) override
    {
        ++calls;
        const auto name = *request.arguments.getIf<Value::Object>()->at("id").getIf<std::string>();
        take(transaction.createObject({take(ObjectId::create(name)), id<ObjectTypeId>("type.stress"), Value{name}}));
        take(transaction.touchRevision(RevisionScope::Geometry));
        if(gate) { gate->arriveAndWait(); }
        return Result<Value>::success(Value{Value::Object{{"id", Value{name}}}});
    }
    std::atomic_uint calls{0U};
    std::shared_ptr<TimedGate> gate;
};

class QueryHandler final : public IQueryHandler {
public:
    Result<Value> execute(const QueryRequest&, const QueryContext& context) override
    {
        ++calls;
        const auto image = context.document.value();
        if(gate) { gate->arriveAndWait(); }
        return Result<Value>::success(Value{Value::Object{
            {"objects", Value{static_cast<std::int64_t>(image.objects().size())}},
            {"revision", Value{std::to_string(image.revisions().at(RevisionScope::Document).value())}}}});
    }
    std::atomic_uint calls{0U};
    std::shared_ptr<TimedGate> gate;
};

class GatedSnapshotStore final : public platform::ISnapshotStore {
public:
    explicit GatedSnapshotStore(std::unique_ptr<platform::ISnapshotStore> store) : store_(std::move(store)) {}
    Result<platform::SnapshotWriteDisposition> writeAtomically(const SnapshotId& key, std::string_view payload) override
    {
        if(armed.load()) { gate.arriveAndWait(); }
        return store_->writeAtomically(key, payload);
    }
    Result<std::string> read(const SnapshotId& key) const override { return store_->read(key); }
    Result<bool> remove(const SnapshotId& key) override { return store_->remove(key); }
    TimedGate gate;
    std::atomic_bool armed{false};
private:
    std::unique_ptr<platform::ISnapshotStore> store_;
};

std::filesystem::path newRoot()
{
    static std::atomic_ullong sequence{0U};
    const auto base = std::filesystem::path{LCNC_STRESS_TEST_ROOT};
    std::filesystem::create_directories(base);
    const auto root = base / (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
        + '-' + std::to_string(sequence.fetch_add(1U)));
    if(!std::filesystem::create_directory(root)) { throw std::runtime_error("Stress evidence directory already exists"); }
    return root;
}

struct Fixture final {
    explicit Fixture(const std::filesystem::path& root, bool seed = true)
    {
        if(seed) { take(kernel.addDocument(project, document)); }
        take(kernel.executionServices().configure(std::make_shared<JsonconsAdapter>(), std::make_shared<NullLog>()));
        take(test::registerObjectType(kernel, test::valueObjectType("type.stress")));
        auto schema = take(Schema::create(id<SchemaId>("schema.stress"), {1U, 0U, 0U}, SchemaKind::Object));
        take(test::registerCommand(kernel, CommandDescriptor{id<CommandName>("test.stress.create"), {1U, 0U, 0U},
            schema, schema, ExecutionMode::Synchronous, SideEffectLevel::DocumentWrite,
            id<CapabilityId>("document.write"), true, true, true}, command));
        take(test::registerQuery(kernel, QueryDescriptor{id<QueryName>("test.stress.read"), {1U, 0U, 0U},
            schema, schema, id<CapabilityId>("document.read"), ExecutionScope::Document, true}, query));
        take(kernel.capabilities().replace(session, std::array{id<CapabilityId>("document.write"), id<CapabilityId>("document.read")}));
        auto snapshotStore = std::make_unique<GatedSnapshotStore>(take(FilesystemSnapshotStore::create({root / "snapshots", 1024U * 1024U})));
        store = snapshotStore.get();
        take(kernel.persistence().configure(take(SqlitePersistenceBackend::open({root / "state.db"})),
            std::make_shared<JsonconsAdapter>(), std::make_shared<Sha256HashService>(), std::move(snapshotStore)));
        take(kernel.bootstrap());
        if(seed) { take(kernel.persistence().captureSnapshot(id<SnapshotId>("snapshot.stress.baseline"),
            take(kernel.documents().snapshot(document)))); }
    }
    AppKernel kernel;
    std::shared_ptr<CreateHandler> command{std::make_shared<CreateHandler>()};
    std::shared_ptr<QueryHandler> query{std::make_shared<QueryHandler>()};
    GatedSnapshotStore* store{nullptr};
};

CommandRequest createRequest(unsigned int index = 0U)
{
    const auto suffix = std::to_string(index);
    return {take(RequestId::create("request.stress." + suffix)), {session, project, document},
        id<CommandName>("test.stress.create"), {1U, 0U, 0U},
        Value{Value::Object{{"id", Value{"object.stress." + suffix}}}}, Revision{},
        id<CorrelationId>("correlation.stress"), id<TraceId>("trace.stress")};
}
QueryRequest queryRequest()
{
    return {id<RequestId>("request.stress.query"), {session, project, document}, id<QueryName>("test.stress.read"),
        {1U, 0U, 0U}, Value{Value::Object{}}, id<CorrelationId>("correlation.stress"), id<TraceId>("trace.stress")};
}
std::size_t active(AppKernel& kernel, DocumentActivityKind kind)
{
    return take(kernel.documentRuntime().lifecycle(document)).activities.at(static_cast<std::size_t>(kind));
}
void verifyState(AppKernel& kernel, std::size_t count)
{
    const auto image = take(kernel.documents().snapshot(document));
    REQUIRE(image.objects().size() == count);
    for(const auto& object : image.objects().all()) {
        REQUIRE(object.type == id<ObjectTypeId>("type.stress"));
        REQUIRE(object.data == Value{std::string(object.id.value())});
        REQUIRE(object.assets.empty());
    }
    for(const auto scope : {RevisionScope::Project, RevisionScope::Document, RevisionScope::Geometry}) {
        REQUIRE(image.revisions().at(scope) == Revision{count});
    }
    for(const auto scope : {RevisionScope::Cam, RevisionScope::MachineContext, RevisionScope::Environment}) {
        REQUIRE(image.revisions().at(scope) == Revision{});
    }
    const auto history = take(kernel.history().snapshot(document));
    REQUIRE(history.cursor == HistoryCursor{count, count});
    REQUIRE(history.entries.size() == count);
    REQUIRE_FALSE(history.barrier.has_value());
    const auto journal = take(kernel.persistence().journalAfter(document, 0U));
    REQUIRE(journal.size() == count);
    for(std::size_t index = 0U; index < journal.size(); ++index) {
        REQUIRE(journal[index].revisionsBefore.at(RevisionScope::Document) == Revision{index});
        REQUIRE(journal[index].revisionsAfter.at(RevisionScope::Document) == Revision{index + 1U});
    }
    for(const auto countActive : take(kernel.documentRuntime().lifecycle(document)).activities) {
        REQUIRE(countActive == 0U);
    }
    REQUIRE(kernel.scheduler().activeTaskCount() == 0U);
}
void verifyQuery(const QueryResponse& response)
{
    REQUIRE(response.result == Value{Value::Object{{"objects", Value{std::int64_t{0}}}, {"revision", Value{"0"}}}});
    REQUIRE(response.revisions == RevisionSet{});
    REQUIRE(response.postExecutionErrors.empty());
}
} // namespace

TEST_CASE("Kernel concurrency queries block close and shutdown without mutating state", "[runtime][stress][f3a]")
{
    for(unsigned int round = 0U; round < rounds; ++round) {
        const auto root = newRoot();
        INFO("round=" << round << " evidence=" << root.string());
        Fixture fixture{root};
        auto& kernel = fixture.kernel;
        auto gate = std::make_shared<TimedGate>();
        fixture.query->gate = gate;
        std::vector<std::future<Result<QueryResponse>>> futures;
        ReleaseOnExit release{*gate};
        for(unsigned int index = 0U; index < 16U; ++index) {
            futures.push_back(std::async(std::launch::async, [&] { return kernel.execution().executeQuery(queryRequest()); }));
        }
        REQUIRE(gate->awaitArrivals(16U));
        REQUIRE(active(kernel, DocumentActivityKind::Query) == 16U);
        const auto closed = kernel.documentRuntime().close(document);
        REQUIRE_FALSE(closed);
        REQUIRE(std::string(closed.error().code.value()) == "Document.ActiveOperations");
        const auto stopped = kernel.shutdown();
        REQUIRE_FALSE(stopped);
        REQUIRE(std::string(stopped.error().code.value()) == "Kernel.ActiveExecutions");
        REQUIRE(kernel.state() == AppKernelState::Ready);
        gate->release();
        for(auto& future : futures) { verifyQuery(take(completed(future))); }
        REQUIRE(fixture.query->calls == 16U);
        verifyState(kernel, 0U);
        REQUIRE(kernel.documentRuntime().close(document));
        REQUIRE(kernel.documentRuntime().open(document));
        verifyState(kernel, 0U);
        REQUIRE(kernel.shutdown());
    }
}

TEST_CASE("Kernel concurrency identical command keys commit once and replay after restart", "[runtime][stress][f3a]")
{
    for(unsigned int round = 0U; round < rounds; ++round) {
        const auto root = newRoot();
        INFO("round=" << round << " evidence=" << root.string());
        auto request = createRequest();
        request.idempotencyKey = id<IdempotencyKey>("key.stress.same");
        std::optional<TransactionId> transaction;
        {
            Fixture fixture{root};
            auto& kernel = fixture.kernel;
            auto gate = std::make_shared<TimedGate>();
            fixture.command->gate = gate;
            std::vector<std::future<Result<CommandResponse>>> futures;
            ReleaseOnExit release{*gate};
            for(unsigned int index = 0U; index < 16U; ++index) {
                futures.push_back(std::async(std::launch::async, [&] { return kernel.execution().executeCommand(request); }));
            }
            REQUIRE(gate->awaitArrivals(1U));
            REQUIRE(awaitCondition([&] { return active(kernel, DocumentActivityKind::Command) == 16U; }));
            REQUIRE(fixture.command->calls == 1U);
            // The Transaction lease covers begin admission, not the candidate lifetime.
            // 中文翻译：Transaction 租约仅覆盖 begin 准入，候选生存期由活动事务表管理。
            REQUIRE(active(kernel, DocumentActivityKind::Transaction) == 0U);
            const auto closed = kernel.documentRuntime().close(document);
            REQUIRE_FALSE(closed);
            REQUIRE(std::string(closed.error().code.value()) == "Document.ActiveOperations");
            const auto stopped = kernel.shutdown();
            REQUIRE_FALSE(stopped);
            REQUIRE(std::string(stopped.error().code.value()) == "Kernel.ActiveTransactions");
            gate->release();
            unsigned int replayed = 0U;
            for(auto& future : futures) {
                const auto response = take(completed(future));
                REQUIRE(response.commit.has_value());
                if(!transaction) { transaction = response.commit->transactionId; }
                REQUIRE(response.commit->transactionId == transaction);
                REQUIRE(response.result == request.arguments);
                REQUIRE(response.postExecutionErrors.empty());
                replayed += response.replayed ? 1U : 0U;
            }
            REQUIRE(replayed == 15U);
            REQUIRE(fixture.command->calls == 1U);
            verifyState(kernel, 1U);
            REQUIRE(kernel.shutdown());
        }
        Fixture recovered{root, false};
        const auto response = take(recovered.kernel.execution().executeCommand(request));
        REQUIRE(response.replayed);
        REQUIRE(response.commit.has_value());
        REQUIRE(response.commit->transactionId == transaction);
        REQUIRE(response.result == request.arguments);
        REQUIRE(recovered.command->calls == 0U);
        verifyState(recovered.kernel, 1U);
        REQUIRE(recovered.kernel.shutdown());
    }
}

TEST_CASE("Kernel concurrency revision competitors leave exactly one durable winner", "[runtime][stress][f3a]")
{
    for(unsigned int round = 0U; round < rounds; ++round) {
        const auto root = newRoot();
        INFO("round=" << round << " evidence=" << root.string());
        std::optional<ObjectId> winner;
        {
            Fixture fixture{root};
            auto& kernel = fixture.kernel;
            auto gate = std::make_shared<TimedGate>();
            fixture.command->gate = gate;
            std::vector<std::future<Result<CommandResponse>>> futures;
            ReleaseOnExit release{*gate};
            for(unsigned int index = 0U; index < 8U; ++index) {
                futures.push_back(std::async(std::launch::async, [&, index] { return kernel.execution().executeCommand(createRequest(index)); }));
            }
            REQUIRE(gate->awaitArrivals(8U));
            REQUIRE(active(kernel, DocumentActivityKind::Command) == 8U);
            REQUIRE(active(kernel, DocumentActivityKind::Transaction) == 0U);
            const auto stopped = kernel.shutdown();
            REQUIRE_FALSE(stopped);
            REQUIRE(std::string(stopped.error().code.value()) == "Kernel.ActiveTransactions");
            REQUIRE(take(kernel.documents().snapshot(document)).objects().size() == 0U);
            gate->release();
            unsigned int failures = 0U;
            for(auto& future : futures) {
                const auto response = completed(future);
                if(response) {
                    REQUIRE_FALSE(winner.has_value());
                    REQUIRE(response.value().commit.has_value());
                    REQUIRE(response.value().commit->changes.size() == 1U);
                    winner = response.value().commit->changes.front().objectId;
                    REQUIRE(response.value().postExecutionErrors.empty());
                } else {
                    REQUIRE(std::string(response.error().code.value()) == "Project.RevisionConflict");
                    ++failures;
                }
            }
            REQUIRE(failures == 7U);
            REQUIRE(winner.has_value());
            REQUIRE(fixture.command->calls == 8U);
            REQUIRE(take(kernel.documents().snapshot(document)).objects().contains(*winner));
            verifyState(kernel, 1U);
            REQUIRE(kernel.shutdown());
        }
        Fixture recovered{root, false};
        REQUIRE(take(recovered.kernel.documents().snapshot(document)).objects().contains(*winner));
        verifyState(recovered.kernel, 1U);
        REQUIRE(recovered.command->calls == 0U);
        REQUIRE(recovered.kernel.shutdown());
    }
}

TEST_CASE("Kernel concurrency closing excludes query admission and permits clean reopen", "[runtime][stress][f3a]")
{
    for(unsigned int round = 0U; round < rounds; ++round) {
        const auto root = newRoot();
        INFO("round=" << round << " evidence=" << root.string());
        Fixture fixture{root};
        auto& kernel = fixture.kernel;
        fixture.store->armed = true;
        auto close = std::async(std::launch::async, [&] { return kernel.documentRuntime().close(document); });
        std::vector<std::future<Result<QueryResponse>>> queries;
        ReleaseOnExit release{fixture.store->gate};
        REQUIRE(fixture.store->gate.awaitArrivals(1U));
        REQUIRE(take(kernel.documentRuntime().lifecycle(document)).state == DocumentLifecycleState::Closing);
        for(unsigned int index = 0U; index < 16U; ++index) {
            queries.push_back(std::async(std::launch::async, [&] { return kernel.execution().executeQuery(queryRequest()); }));
        }
        for(auto& future : queries) {
            const auto result = completed(future);
            REQUIRE_FALSE(result);
            REQUIRE(std::string(result.error().code.value()) == "Document.NotOpen");
        }
        REQUIRE(fixture.query->calls == 0U);
        fixture.store->gate.release();
        REQUIRE(take(completed(close)).state == DocumentLifecycleState::Detached);
        REQUIRE(kernel.documentRuntime().open(document));
        verifyQuery(take(kernel.execution().executeQuery(queryRequest())));
        REQUIRE(fixture.query->calls == 1U);
        verifyState(kernel, 0U);
        REQUIRE(kernel.shutdown());
    }
}
