#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Psapi.h>

#include "kernel_test_module.hpp"
#include <lasercnc/infrastructure/filesystem_snapshot_store.hpp>
#include <lasercnc/infrastructure/jsoncons_adapter.hpp>
#include <lasercnc/infrastructure/sha256_hash_service.hpp>
#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
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
using Clock = std::chrono::steady_clock;

void check(bool condition, const char* message)
{ if(!condition) { throw std::runtime_error(message); } }
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
Value number(std::size_t value) { return Value{static_cast<std::int64_t>(value)}; }
double elapsed(Clock::time_point start)
{ return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

const auto project = id<ProjectId>("project.benchmark");
const auto document = id<DocumentId>("document.benchmark");
const auto session = id<SessionId>("session.benchmark");
const auto type = id<ObjectTypeId>("type.benchmark.record");
const auto capability = id<CapabilityId>("benchmark.execute");

ObjectId objectId(std::size_t index)
{
    const auto digits = std::to_string(index);
    return take(ObjectId::create("object.benchmark." + std::string(9U - digits.size(), '0') + digits));
}
const auto firstObject = objectId(0U);
Value payload(char fill = 'a') { return Value{Value::Object{{"payload", Value{std::string(128U, fill)}}}}; }
char changedFill(std::size_t iteration) { return iteration % 2U == 0U ? 'b' : 'a'; }

class PayloadValidator final : public IObjectTypeValidator {
public:
    Result<void> validate(const Value& value) const override
    {
        const auto* object = value.getIf<Value::Object>();
        if(object && object->size() == 1U && object->contains("payload")) {
            const auto* text = object->at("payload").getIf<std::string>();
            if(text && text->size() == 128U) { return Result<void>::success(); }
        }
        return Result<void>::failure(makeError("Benchmark.InvalidPayload", ErrorCategory::Validation,
            "Expected one fixed-size payload field"));
    }
};
ObjectTypeDefinition objectType()
{
    return {{type, {1U, 0U, 0U}, ObjectPersistencePolicy::Durable},
        {{{1U, 0U, 0U}, std::make_shared<PayloadValidator>(), std::make_shared<test::TestEmptyObjectReferences>()}}, {}};
}
std::vector<ObjectRecord> objects(std::size_t count)
{
    std::vector<ObjectRecord> result;
    result.reserve(count);
    for(std::size_t index = 0U; index < count; ++index) { result.push_back({objectId(index), type, payload()}); }
    return result;
}
void verifyObjects(std::span<const ObjectRecord> records, std::size_t count, char first)
{
    check(records.size() == count, "Object count changed");
    const auto baseline = payload();
    const auto changed = payload(first);
    for(std::size_t index = 0U; index < count; ++index) {
        check(records[index].id == objectId(index) && records[index].type == type
            && records[index].schemaVersion == Version{1U, 0U, 0U} && records[index].assets.empty()
            && records[index].data == (index == 0U ? changed : baseline), "Object material changed");
    }
}
void verifyRevisions(const RevisionSet& revisions, std::size_t revision)
{
    check(revisions.at(RevisionScope::Project) == Revision{revision}
        && revisions.at(RevisionScope::Document) == Revision{revision}, "Revision changed unexpectedly");
    for(const auto scope : {RevisionScope::Geometry, RevisionScope::Cam, RevisionScope::MachineContext, RevisionScope::Environment}) {
        check(revisions.at(scope) == Revision{}, "Unrelated revision changed");
    }
}
void verifyDocument(const Document& image, std::size_t count, char first, std::size_t revision)
{
    check(image.id() == document && image.projectId() == project, "Document ownership changed");
    verifyRevisions(image.revisions(), revision);
    verifyObjects(image.objects().all(), count, first);
}

Value memory()
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    check(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
        sizeof(counters)) != FALSE, "Cannot read process memory");
    return Value{Value::Object{{"working_set_bytes", number(counters.WorkingSetSize)},
        {"private_bytes", number(counters.PrivateUsage)}, {"process_peak_working_set_bytes", number(counters.PeakWorkingSetSize)}}};
}

struct Options final {
    std::size_t count{1000U};
    std::size_t samples{5U};
    std::size_t warmup{1U};
    std::size_t cycles{10U};
    std::string family{"component"};
    std::string storage{"memory"};
    std::filesystem::path base{LCNC_BENCH_ROOT};
    bool durable() const { return storage == "sqlite"; }
};
std::size_t integer(const std::string& value)
{
    check(!value.empty() && std::all_of(value.begin(), value.end(), [](char ch) { return ch >= '0' && ch <= '9'; }),
        "Expected an unsigned integer");
    return static_cast<std::size_t>(std::stoull(value));
}
Options options(int argc, char** argv)
{
    Options result;
    for(int index = 1; index < argc; index += 2) {
        check(index + 1 < argc, "Missing option value");
        const std::string key = argv[index];
        const std::string value = argv[index + 1];
        if(key == "--objects") { result.count = integer(value); }
        else if(key == "--samples") { result.samples = integer(value); }
        else if(key == "--warmup") { result.warmup = integer(value); }
        else if(key == "--cycles") { result.cycles = integer(value); }
        else if(key == "--family") { result.family = value; }
        else if(key == "--storage") { result.storage = value; }
        else if(key == "--output-root") { result.base = std::filesystem::path{value}; }
        else { throw std::runtime_error("Unknown benchmark option: " + key); }
    }
    check(result.count == 10U || result.count == 1000U || result.count == 10000U || result.count == 100000U,
        "Object count must be 10 (smoke), 1000, 10000 or 100000");
    check(result.samples >= 2U && result.samples <= 64U && result.warmup <= 10U
        && result.cycles >= 3U && result.cycles <= 30U, "Sampling limits exceeded");
    check(result.storage == "memory" || result.storage == "sqlite", "Unknown storage mode");
    check(result.family == "component" || result.family == "gateway" || result.family == "journal"
        || result.family == "lifecycle", "Unknown family");
    check(result.family != "journal" || result.durable(), "Journal measurements require actual SQLite");
    check(result.base.is_absolute(), "Output root must be absolute");
    return result;
}

struct Report final {
    explicit Report(Options settings) : opts(std::move(settings))
    {
        std::filesystem::create_directories(opts.base);
        root = opts.base / (opts.family + '-' + opts.storage + '-' + std::to_string(opts.count) + '-'
            + std::to_string(GetCurrentProcessId()) + '-' + std::to_string(Clock::now().time_since_epoch().count()));
        check(std::filesystem::create_directory(root), "Evidence directory already exists");
        MEMORYSTATUSEX system{};
        system.dwLength = sizeof(system);
        check(GlobalMemoryStatusEx(&system) != FALSE, "Cannot read physical memory");
        metadata = {{"schema_version", number(1U)}, {"family", Value{opts.family}}, {"storage", Value{opts.storage}},
            {"object_count", number(opts.count)}, {"value_serialized_bytes", number(take(JsonconsAdapter{}.serialize(payload())).size())},
            {"payload_string_bytes", number(128U)}, {"assets_per_object", number(0U)},
            {"samples", number(opts.samples)}, {"warmup", number(opts.warmup)}, {"cycles", number(opts.cycles)},
            {"build_config", Value{LCNC_BENCH_CONFIG}}, {"compiler", Value{LCNC_BENCH_COMPILER}},
            {"logical_processors", number(std::thread::hardware_concurrency())},
            {"physical_memory_bytes", number(static_cast<std::size_t>(system.ullTotalPhys))},
            {"initial_memory", memory()}, {"output_directory", Value{root.generic_string()}},
            {"timing_note", Value{"操作计时不包含准备、结果校验和返回对象析构；缓存不清空。进程峰值含初始化及之前用例，不等于单操作峰值。"}}};
    }

    template<typename Prepare, typename Operation, typename Verify>
    void measure(const char* name, Prepare prepare, Operation operation, Verify verify)
    {
        Value::Array records;
        std::vector<double> times;
        for(std::size_t iteration = 0U; iteration < opts.warmup + opts.samples; ++iteration) {
            prepare(iteration);
            auto before = memory();
            const auto start = Clock::now();
            auto retained = operation(iteration);
            const auto milliseconds = elapsed(start);
            auto held = memory();
            const auto verifyStart = Clock::now();
            verify(retained, iteration);
            const auto verification = elapsed(verifyStart);
            if(iteration >= opts.warmup) {
                times.push_back(milliseconds);
                records.push_back(Value{Value::Object{{"iteration", number(iteration - opts.warmup)},
                    {"elapsed_ms", Value{milliseconds}}, {"verification_ms", Value{verification}},
                    {"memory_before", std::move(before)}, {"memory_result_live", std::move(held)}}});
            }
        }
        std::sort(times.begin(), times.end());
        const auto middle = times.size() / 2U;
        const auto median = times.size() % 2U == 0U ? (times[middle - 1U] + times[middle]) / 2.0 : times[middle];
        rows.push_back(Value{Value::Object{{"operation", Value{name}}, {"min_ms", Value{times.front()}},
            {"median_ms", Value{median}}, {"max_ms", Value{times.back()}}, {"raw_samples", Value{std::move(records)}}}});
        std::cout << name << ": median_ms=" << median << '\n' << std::flush;
    }
    template<typename Operation, typename Verify>
    void measure(const char* name, Operation operation, Verify verify)
    { measure(name, [](std::size_t) {}, std::move(operation), std::move(verify)); }

    void save()
    {
        std::uintmax_t bytes = 0U;
        for(const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if(entry.is_regular_file()) { bytes += entry.file_size(); }
        }
        metadata.emplace("storage_files_bytes_before_report", number(static_cast<std::size_t>(bytes)));
        metadata.emplace("final_memory", memory());
        const auto encoded = take(JsonconsAdapter{}.serialize(Value{Value::Object{
            {"metadata", Value{metadata}}, {"operations", Value{rows}}, {"lifecycle", Value{lifecycle}}}}));
        std::ofstream output(root / "report.json", std::ios::binary);
        output << encoded << '\n';
        output.close();
        check(!output.fail(), "Cannot write benchmark report");
        std::cout << "benchmark-verified: " << (root / "report.json").generic_string() << '\n';
    }
    Options opts;
    std::filesystem::path root;
    Value::Object metadata;
    Value::Array rows;
    Value::Array lifecycle;
};

void configurePersistence(persistence::PersistenceService& persistence, const std::filesystem::path& root,
    Report& report)
{
    std::filesystem::create_directories(root);
    auto backend = take(SqlitePersistenceBackend::open({root / "state.db"}));
    if(!report.metadata.contains("sqlite_settings")) {
        Value::Object settings;
        for(const auto* pragma : {"journal_mode", "synchronous", "page_size", "cache_size", "foreign_keys"}) {
            const auto result = take(backend->query(std::string("PRAGMA ") + pragma));
            check(result.size() == 1U && result.front().size() == 1U, "Unexpected PRAGMA response");
            settings.emplace(pragma, result.front().begin()->second);
        }
        report.metadata.emplace("sqlite_settings", Value{std::move(settings)});
    }
    take(persistence.configure(std::move(backend), std::make_shared<JsonconsAdapter>(),
        std::make_shared<Sha256HashService>(), take(FilesystemSnapshotStore::create({root / "snapshots", 256U * 1024U * 1024U}))));
}

class NullLog final : public observability::ILogService {
public:
    Result<void> write(const observability::LogRecord&) override { return Result<void>::success(); }
    Result<void> flush() override { return Result<void>::success(); }
};
class ReadHandler final : public IQueryHandler, public IReadOnlyCommandHandler {
public:
    Result<Value> execute(const QueryRequest&, const QueryContext& context) override
    { return Result<Value>::success(Value{Value::Object{{"count", number(context.document ? context.document->objects().size() : 0U)}}}); }
    Result<Value> execute(const CommandRequest&, const ReadOnlyCommandContext& context) override
    { return Result<Value>::success(Value{Value::Object{{"count", number(context.document ? context.document->objects().size() : 0U)}}}); }
};
class UpdateHandler final : public ICommandHandler {
public:
    Result<Value> execute(const CommandRequest& request, ApplicationTransaction& transaction) override
    {
        take(transaction.replaceObjectData(firstObject, request.arguments));
        return Result<Value>::success(Value{Value::Object{{"changed", Value{true}}}});
    }
};

std::unique_ptr<AppKernel> configuredKernel(Report& report, const std::filesystem::path& root, bool durable)
{
    auto kernel = std::make_unique<AppKernel>();
    take(kernel->executionServices().configure(std::make_shared<JsonconsAdapter>(), std::make_shared<NullLog>()));
    take(test::registerObjectType(*kernel, objectType()));
    const auto schema = take(Schema::create(id<SchemaId>("schema.benchmark"), {1U, 0U, 0U}, SchemaKind::Object));
    auto read = std::make_shared<ReadHandler>();
    for(const bool global : {true, false}) {
        const auto scope = global ? ExecutionScope::Global : ExecutionScope::Document;
        take(test::registerQuery(*kernel, QueryDescriptor{id<QueryName>(global ? "bench.global.read" : "bench.document.read"),
            {1U, 0U, 0U}, schema, schema, capability, scope, true}, read));
        take(test::registerReadOnlyCommand(*kernel, CommandDescriptor{id<CommandName>(global ? "bench.global.noop" : "bench.document.noop"),
            {1U, 0U, 0U}, schema, schema, ExecutionMode::Synchronous, SideEffectLevel::ReadOnly,
            capability, false, true, false, ContractStatus::Active, scope}, read));
    }
    take(test::registerCommand(*kernel, CommandDescriptor{id<CommandName>("bench.update"), {1U, 0U, 0U}, schema, schema,
        ExecutionMode::Synchronous, SideEffectLevel::DocumentWrite, capability, true, true, false}, std::make_shared<UpdateHandler>()));
    take(kernel->capabilities().replace(session, std::array{capability, id<CapabilityId>("kernel.history.edit")}));
    if(durable) { configurePersistence(kernel->persistence(), root, report); }
    return kernel;
}
void seedKernel(AppKernel& kernel, Report& report, bool durable)
{
    const auto start = Clock::now();
    take(kernel.bootstrap());
    take(kernel.documentRuntime().attach({project, document, {}, objects(report.opts.count)}));
    if(durable) {
        take(kernel.persistence().captureSnapshot(id<SnapshotId>("snapshot.benchmark.baseline"), take(kernel.documents().snapshot(document))));
    }
    report.metadata.insert_or_assign("last_seed_ms", Value{elapsed(start)});
}
CommandRequest command(const char* name, bool global = false, std::size_t sequence = 0U)
{
    CommandRequest request{take(RequestId::create("request.benchmark." + std::to_string(sequence))), {session, project, document},
        id<CommandName>(name), {1U, 0U, 0U}, Value{Value::Object{}}, std::nullopt,
        id<CorrelationId>("correlation.benchmark"), id<TraceId>("trace.benchmark")};
    if(global) { request.context = {session, std::nullopt, std::nullopt}; }
    if(std::string_view{name} == "bench.update") { request.arguments = payload(changedFill(sequence)); }
    return request;
}
QueryRequest query(bool global)
{
    return {id<RequestId>("request.benchmark.query"), global ? ExecutionContext{session, std::nullopt, std::nullopt}
        : ExecutionContext{session, project, document}, id<QueryName>(global ? "bench.global.read" : "bench.document.read"),
        {1U, 0U, 0U}, Value{Value::Object{}}, id<CorrelationId>("correlation.benchmark"), id<TraceId>("trace.benchmark")};
}
void verifyCount(const Value& value, std::size_t count)
{ check(value == Value{Value::Object{{"count", number(count)}}}, "Read result changed"); }

void component(Report& report)
{
    DocumentStore store;
    ObjectTypeRegistry types;
    take(types.registerType(objectType()));
    types.freeze();
    persistence::PersistenceService persistence;
    if(report.opts.durable()) { configurePersistence(persistence, report.root / "component", report); take(persistence.initialize()); }
    TransactionManager transactions{store, report.opts.durable() ? &persistence : nullptr, nullptr, nullptr, &types};
    const auto start = Clock::now();
    take(store.addDocument(project, document));
    {
        auto seed = take(transactions.begin(id<TransactionId>("transaction.benchmark.seed"), document));
        for(auto& record : objects(report.opts.count)) { take(seed->createObject(std::move(record))); }
        take(seed->commit());
    }
    report.metadata.emplace("seed_ms", Value{elapsed(start)});
    report.metadata.emplace("seed_memory", memory());
    report.measure("document.snapshot", [&](std::size_t) { return take(store.snapshot(document)); },
        [&](const auto& image, std::size_t) { verifyDocument(image, report.opts.count, 'a', 1U); });
    report.measure("transaction.begin", [&](std::size_t index) {
        return take(transactions.begin(take(TransactionId::create("transaction.benchmark.begin." + std::to_string(index))), document));
    }, [&](auto& transaction, std::size_t) {
        verifyObjects(transaction->stagedObjects().all(), report.opts.count, 'a');
        take(transaction->rollback());
        check(transactions.activeTransactionCount() == 0U, "Transaction lease leaked");
    });
    std::unique_ptr<ApplicationTransaction> candidate;
    report.measure("transaction.commit", [&](std::size_t index) {
        candidate = take(transactions.begin(take(TransactionId::create("transaction.benchmark.commit." + std::to_string(index))), document));
        take(candidate->replaceObjectData(firstObject, payload(changedFill(index))));
    }, [&](std::size_t) { return take(candidate->commit()); }, [&](const auto& receipt, std::size_t index) {
        check(receipt.changes.size() == 1U, "Commit delta changed");
        verifyDocument(take(store.snapshot(document)), report.opts.count, changedFill(index), index + 2U);
        check(transactions.activeTransactionCount() == 0U, "Committed transaction remains active");
        candidate.reset();
    });
}

void gateway(Report& report)
{
    auto kernel = configuredKernel(report, report.root / "gateway", report.opts.durable());
    seedKernel(*kernel, report, report.opts.durable());
    report.metadata.emplace("seed_memory", memory());
    ReadHandler direct;
    const auto globalQuery = query(true);
    report.measure("query.direct_handler", [&](std::size_t) { return take(direct.execute(globalQuery, QueryContext{})); },
        [](const Value& value, std::size_t) { verifyCount(value, 0U); });
    const auto globalCommand = command("bench.global.noop", true);
    report.measure("command.direct_handler", [&](std::size_t) { return take(direct.execute(globalCommand, ReadOnlyCommandContext{})); },
        [](const Value& value, std::size_t) { verifyCount(value, 0U); });
    for(const bool global : {true, false}) {
        const auto request = query(global);
        report.measure(global ? "query.global_gateway" : "query.document_gateway",
            [&](std::size_t) { return take(kernel->execution().executeQuery(request)); },
            [&](const QueryResponse& response, std::size_t) {
                verifyCount(response.result, global ? 0U : report.opts.count);
                check(response.postExecutionErrors.empty(), "Query integration error");
                if(!global) { check(response.revisions == RevisionSet{}, "Query mutated revisions"); }
            });
        const auto noop = command(global ? "bench.global.noop" : "bench.document.noop", global);
        report.measure(global ? "command.global_gateway" : "command.document_readonly_gateway",
            [&](std::size_t) { return take(kernel->execution().executeCommand(noop)); },
            [&](const CommandResponse& response, std::size_t) {
                verifyCount(response.result, global ? 0U : report.opts.count);
                check(!response.commit && response.postExecutionErrors.empty(), "Read-only command produced side effects");
            });
    }
    CommandRequest update = command("bench.update");
    report.measure("command.document_write_gateway", [&](std::size_t index) { update = command("bench.update", false, index); },
        [&](std::size_t) { return take(kernel->execution().executeCommand(update)); },
        [&](const CommandResponse& response, std::size_t index) {
            check(response.commit && response.commit->changes.size() == 1U && response.postExecutionErrors.empty(), "Write outcome changed");
            verifyDocument(take(kernel->documents().snapshot(document)), report.opts.count, changedFill(index), index + 1U);
        });
    const auto extent = report.opts.warmup + report.opts.samples;
    const auto finalFill = changedFill(extent - 1U);
    const auto undoFill = finalFill == 'a' ? 'b' : 'a';
    auto revision = extent;
    auto historySequence = extent;
    auto historyCall = [&](bool undo) {
        auto response = take(kernel->execution().executeCommand(command(undo ? "edit.undo" : "edit.redo", false, historySequence++)));
        ++revision;
        check(response.commit && response.postExecutionErrors.empty(), "History execution failed");
        return response;
    };
    for(const bool undo : {true, false}) {
        report.measure(undo ? "history.undo_gateway" : "history.redo_gateway", [&](std::size_t) {
            const auto cursor = take(kernel->history().snapshot(document)).cursor;
            if((undo && cursor.position != extent) || (!undo && cursor.position == extent)) { historyCall(!undo); }
        }, [&](std::size_t) { return historyCall(undo); }, [&](const CommandResponse&, std::size_t) {
            verifyDocument(take(kernel->documents().snapshot(document)), report.opts.count, undo ? undoFill : finalFill, revision);
            check(take(kernel->history().snapshot(document)).cursor == HistoryCursor{undo ? extent - 1U : extent, extent},
                "History cursor changed");
        });
    }
    report.metadata.emplace("retained_history_entries", number(extent));
    take(kernel->shutdown());
}

void journal(Report& report)
{
    const auto storageRoot = report.root / "journal";
    auto source = configuredKernel(report, report.root / "source", false);
    seedKernel(*source, report, false);
    persistence::PersistenceService target;
    configurePersistence(target, storageRoot, report);
    take(target.initialize());
    take(target.saveDocumentLifecycle(project, document, persistence::DocumentPersistenceState::Open));
    take(target.captureSnapshot(id<SnapshotId>("snapshot.benchmark.baseline"), take(source->documents().snapshot(document))));
    report.metadata.emplace("snapshot_objects", number(report.opts.count));
    std::optional<TransactionCommit> receipt;
    report.measure("journal.append", [&](std::size_t index) {
        receipt = take(source->execution().executeCommand(command("bench.update", false, index))).commit;
        check(receipt.has_value(), "Missing source commit");
    }, [&](std::size_t) { return take(target.append(*receipt)); }, [&](const auto& record, std::size_t index) {
        check(record.sequence == index + 1U && record.transactionId == receipt->transactionId, "Journal sequence changed");
        verifyRevisions(record.revisionsAfter, index + 1U);
    });
    const auto tail = report.opts.warmup + report.opts.samples;
    report.metadata.emplace("journal_tail_records", number(tail));
    take(source->shutdown());
    source.reset();
    receipt.reset();
    report.measure("journal.recover_material", [&](std::size_t) { return take(target.recover()); },
        [&](const persistence::RecoveryReport& recovered, std::size_t) {
            check(recovered.documents.size() == 1U && recovered.journalRecordsReplayed == tail
                && recovered.historyCommits.size() == tail, "Recovery extent changed");
            verifyObjects(recovered.documents.front().objects, report.opts.count, changedFill(tail - 1U));
            verifyRevisions(recovered.documents.front().revisions, tail);
        });
    std::unique_ptr<AppKernel> recovered;
    report.measure("kernel.bootstrap_recovery", [&](std::size_t) { recovered = configuredKernel(report, storageRoot, true); },
        [&](std::size_t) { take(recovered->bootstrap()); return true; }, [&](bool, std::size_t) {
            verifyDocument(take(recovered->documents().snapshot(document)), report.opts.count, changedFill(tail - 1U), tail);
            check(take(recovered->history().snapshot(document)).cursor == HistoryCursor{tail, tail}, "Recovered history changed");
            take(recovered->shutdown());
            recovered.reset();
        });
}

void lifecycle(Report& report)
{
    for(std::size_t cycle = 0U; cycle < report.opts.cycles; ++cycle) {
        auto before = memory();
        const auto start = Clock::now();
        auto kernel = configuredKernel(report, report.root / ("cycle-" + std::to_string(cycle)), report.opts.durable());
        seedKernel(*kernel, report, report.opts.durable());
        const auto startup = elapsed(start);
        auto live = memory();
        verifyDocument(take(kernel->documents().snapshot(document)), report.opts.count, 'a', 0U);
        const auto stop = Clock::now();
        take(kernel->shutdown());
        kernel.reset();
        const auto teardown = elapsed(stop);
        report.lifecycle.push_back(Value{Value::Object{{"cycle", number(cycle)}, {"startup_seed_ms", Value{startup}},
            {"shutdown_destroy_ms", Value{teardown}}, {"memory_before", std::move(before)},
            {"memory_live", std::move(live)}, {"memory_after_destroy", memory()}}});
    }
}
} // namespace

int main(int argc, char** argv)
{
    try {
        Report report{options(argc, argv)};
        if(report.opts.family == "component") { component(report); }
        else if(report.opts.family == "gateway") { gateway(report); }
        else if(report.opts.family == "journal") { journal(report); }
        else { lifecycle(report); }
        report.save();
        return 0;
    } catch(const std::exception& exception) {
        std::cerr << "benchmark-failed: " << exception.what() << '\n';
        return 1;
    }
}
