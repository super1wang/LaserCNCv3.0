#include <lasercnc/infrastructure/bs_thread_pool_executor.hpp>
#include <lasercnc/infrastructure/sqlite_persistence_backend.hpp>
#include <lasercnc/infrastructure/jsoncons_adapter.hpp>
#include <lasercnc/infrastructure/sha256_hash_service.hpp>
#include "kernel_test_module.hpp"
#include <array>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <thread>

using namespace lasercnc;
using namespace std::chrono_literals;
namespace {
void check(bool condition, const char* message) { if(!condition) { throw std::runtime_error(message); } }
template<typename Id> Id id(const char* text) { return Id::create(text).value(); }
class NullLog final : public observability::ILogService {
public:
    foundation::Result<void> write(const observability::LogRecord&) override { return foundation::Result<void>::success(); }
    foundation::Result<void> flush() override { return foundation::Result<void>::success(); }
};
class Handler final : public runtime::ITaskHandler {
public:
    std::function<void()> run;
    foundation::Result<foundation::Value> execute(const runtime::TaskRequest&, const runtime::TaskContext&) override
    { run(); return foundation::Result<foundation::Value>::success(foundation::Value{}); }
};
class Exporter final : public observability::ITraceExporter {
public:
    std::function<void()> finished;
    foundation::Result<void> exportSpan(const observability::TraceSpanRecord& span) override
    {
        if(span.name == "task.execute") { finished(); }
        return foundation::Result<void>::success();
    }
};
class StopWitness final : public kernel::IModule {
public:
    std::function<void(kernel::AppKernel&)> stopped;
    std::function<void()> starting;
    const kernel::ModuleDescriptor& descriptor() const noexcept override { return descriptor_; }
    foundation::Result<void> start(kernel::AppKernel&) override
    { if(starting) { starting(); } return foundation::Result<void>::success(); }
    foundation::Result<void> stop(kernel::AppKernel& host) override
    { stopped(host); return foundation::Result<void>::success(); }
private:
    kernel::ModuleDescriptor descriptor_{id<kernel::ModuleId>("module.drain-witness"), "Drain witness", {1U, 0U, 0U}};
};
class ObservedExecutor final : public platform::ITaskExecutor {
public:
    std::unique_ptr<infrastructure::BsThreadPoolExecutor> inner =
        infrastructure::BsThreadPoolExecutor::create({1U}).value();
    bool notify{true};
    ~ObservedExecutor() override { drainForDestruction(); }
    foundation::Result<void> submit(platform::ExecutorWork work, platform::ExecutorCompletion done) override
    { return inner->submit(std::move(work), std::move(done)); }
    foundation::Result<void> waitIdle() override { return inner->waitIdle(); }
    foundation::Result<void> shutdown() override { return inner->shutdown(); }
    void drainForDestruction() noexcept override
    {
        if(notify) { notify = false; std::cout << "draining" << std::endl; }
        inner->drainForDestruction();
    }
    bool isCurrentWorkerThread() const noexcept override { return inner->isCurrentWorkerThread(); }
    std::size_t concurrency() const noexcept override { return inner->concurrency(); }
};
void configurePersistence(kernel::AppKernel& host, const std::filesystem::path& path)
{
    auto backend = infrastructure::SqlitePersistenceBackend::open({path});
    check(backend.hasValue(), "open database");
    check(host.configurePersistence(std::move(backend).value(),
        std::make_shared<infrastructure::JsonconsAdapter>(),
        std::make_shared<infrastructure::Sha256HashService>()).hasValue(), "configure persistence");
}
bool denied(const std::filesystem::path& path)
{
    kernel::AppKernel second;
    configurePersistence(second, path);
    const auto result = second.bootstrap();
    if(result) { return false; }
    for(auto* error = &result.error(); error != nullptr; error = error->cause.get()) {
        if(error->code.value() == "Persistence.HostAlreadyOwned") { return true; }
    }
    throw std::runtime_error("unexpected admission failure");
}
void handshake(const char* expected)
{
    std::string input;
    check(static_cast<bool>(std::getline(std::cin, input)), "missing handshake");
    // Windows PowerShell can emit a BOM when its redirected stdin writer initializes.
    // 中文翻译：Windows PowerShell 初始化重定向输入写入器时可能输出 BOM。
    if(input.starts_with("\xEF\xBB\xBF")) {
        std::cerr << "stdin-bom-normalized" << std::endl;
        input.erase(0U, 3U);
    }
    check(input == expected, "invalid handshake");
}
const auto taskId = id<kernel::TaskId>("task.final-drain");
const auto session = id<kernel::SessionId>("session.final-drain");
void configureTask(kernel::AppKernel& host, std::shared_ptr<Handler> handler)
{
    const std::array grants{id<kernel::CapabilityId>("task.final-drain.submit")};
    check(host.capabilities().replace(session, grants).hasValue(), "grant");
    check(host.executionServices().configure(std::make_shared<infrastructure::JsonconsAdapter>(),
        std::make_shared<NullLog>()).hasValue(), "services");
    runtime::TaskRequest task{taskId, id<kernel::TaskName>("task.final-drain"), foundation::Value{},
        id<kernel::TraceId>("trace.final-drain")};
    task.resources.push_back({runtime::ResourceKind::DiskIO, id<kernel::ResourceId>("resource.final-drain"),
        runtime::ResourceAccess::Exclusive, 1U});
    check(host.resources().configure(runtime::ResourceKind::DiskIO,
        id<kernel::ResourceId>("resource.final-drain"), 1U).hasValue(), "resource");
    auto command = test::taskSubmissionDescriptor("command.final-drain", "task.final-drain.submit");
    command.idempotent = false;
    check(test::registerAsyncCommand(host, std::move(command),
        std::make_shared<test::FixedTaskCommandHandler>(task)).hasValue(), "command");
    check(test::registerTask(host, {task.task, {1U, 0U, 0U}, test::testAnySchema("schema.drain.input"),
        test::testAnySchema("schema.drain.output")}, std::move(handler)).hasValue(), "task");
}
void submit(kernel::AppKernel& host)
{
    check(host.execution().executeCommand(test::taskSubmissionRequest("request.final-drain", "command.final-drain",
        session, "trace.final-drain.command")).hasValue(), "submit");
}
int hold(const std::filesystem::path& path)
{
    std::promise<void> entered;
    auto entry = entered.get_future();
    std::atomic_bool workVerified{false}, observationVerified{false};
    bool stopVerified = false;
    auto host = std::make_unique<kernel::AppKernel>();
    auto* live = host.get();
    configurePersistence(*host, path);
    check(host->addDocument(id<kernel::ProjectId>("project.final-drain"),
        id<kernel::DocumentId>("document.final-drain")).hasValue(), "document");
    auto handler = std::make_shared<Handler>();
    handler->run = [&] {
        entered.set_value();
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        const auto releasePath = path.parent_path() / "release";
        while(!std::filesystem::exists(releasePath) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        check(std::filesystem::exists(releasePath), "task gate expired");
        std::cerr << "task-released" << std::endl;
        check(live->state() == kernel::AppKernelState::Stopping, "host not stopping");
        check(live->execution().task(taskId).hasValue(), "task runtime gone");
        check(live->execution().catalog().tasks.size() == 1U, "gateway registry gone");
        std::cerr << "gateway-read" << std::endl;
        check(live->projectRuntime().catalog().value().entries.size() == 1U, "project runtime gone");
        check(live->documentRuntime().catalog().value().entries.size() == 1U, "document runtime gone");
        check(!live->execution().workflow(id<kernel::WorkflowId>("workflow.missing")), "workflow read");
        check(!live->execution().script(id<kernel::ScriptExecutionId>("script.missing")), "script read");
        std::cerr << "runtimes-read" << std::endl;
        check(live->resources().snapshot().front().exclusivelyHeld, "resource released early");
        check(live->persistence().sessionStatus().ready && denied(path), "same-process ownership lost");
        std::cerr << "same-process-denied" << std::endl;
        workVerified.store(true);
    };
    configureTask(*host, handler);
    auto witness = std::make_unique<StopWitness>();
    witness->stopped = [&](kernel::AppKernel& stoppingHost) {
        check(workVerified.load() && observationVerified.load(), "module stopped before callbacks");
        const auto history = stoppingHost.persistence().taskHistory(taskId);
        check(history.hasValue() && history.value().has_value()
            && history.value()->state == runtime::TaskState::Cancelled, "terminal history missing before module stop");
        check(stoppingHost.scheduler().activeTaskCount() == 0U, "tasks active at module stop");
        stopVerified = true;
    };
    check(host->addModule(std::move(witness)).hasValue(), "witness module");
    auto exporter = std::make_shared<Exporter>();
    exporter->finished = [&] {
        std::cerr << "observation-entered" << std::endl;
        check(live->execution().task(taskId).hasValue(), "completion runtime gone");
        check(live->state() == kernel::AppKernelState::Stopping, "completion after stop");
        check(!live->resources().snapshot().front().exclusivelyHeld, "resource not released by completion");
        observationVerified.store(true);
    };
    check(host->traces().addExporter(exporter).hasValue(), "exporter");
    check(host->configureTaskExecutor(std::make_unique<ObservedExecutor>()).hasValue(), "executor");
    check(host->bootstrap().hasValue(), "bootstrap");
    submit(*host);
    check(entry.wait_for(5s) == std::future_status::ready, "no worker");
    const auto stop = host->shutdown(2ms);
    check(!stop && stop.error().code.value() == "Task.ShutdownTimeout", "short stop was not a timeout");
    check(denied(path), "same-process early takeover");
    std::cout << "ready" << std::endl;
    handshake("destroy");
    host.reset();
    std::cerr << "host-reset-returned" << std::endl;
    check(workVerified.load() && observationVerified.load() && stopVerified, "missing lifetime observation");
    check(!denied(path), "same-process takeover after destruction failed");
    std::cout << "destroyed" << std::endl;
    handshake("exit");
    return 0;
}
int selfDestroy(bool hostMode)
{
    std::promise<void> released;
    auto release = released.get_future().share();
    if(hostMode) {
        std::promise<void> entered;
        auto entry = entered.get_future();
        auto host = std::make_unique<kernel::AppKernel>();
        auto handler = std::make_shared<Handler>();
        handler->run = [&] {
            std::set_terminate([] { std::_Exit(91); });
            entered.set_value(); release.wait();
            std::cerr << "self-host-delete=" << static_cast<bool>(host) << std::endl;
            host.reset();
            std::cerr << "self-host-delete-returned" << std::endl;
        };
        configureTask(*host, handler);
        check(host->configureTaskExecutor(infrastructure::BsThreadPoolExecutor::create({1U}).value()).hasValue(), "executor");
        check(host->bootstrap().hasValue(), "bootstrap");
        submit(*host);
        if(entry.wait_for(5s) != std::future_status::ready) {
            const auto task = host->execution().task(taskId);
            if(task && task.value().error) { std::cerr << task.value().error->code.value() << ':' << task.value().error->message << std::endl; }
            std::_Exit(5);
        }
        released.set_value();
        std::this_thread::sleep_for(10s);
    } else {
        auto executor = infrastructure::BsThreadPoolExecutor::create({1U}).value();
        check(executor->submit([&] {
            std::set_terminate([] { std::_Exit(91); });
            release.wait();
            std::cerr << "self-executor-delete" << std::endl;
            executor.reset(); return foundation::Result<void>::success(); },
            [](foundation::Result<void>) {}).hasValue(), "submit");
        released.set_value();
        std::this_thread::sleep_for(10s);
    }
    return 7;
}
int activeBootstrap()
{
    auto host = std::make_unique<kernel::AppKernel>();
    std::promise<void> entered;
    auto entry = entered.get_future();
    std::promise<void> blocked;
    auto block = blocked.get_future().share();
    auto module = std::make_unique<StopWitness>();
    module->starting = [&] { entered.set_value(); block.wait(); };
    module->stopped = [](kernel::AppKernel&) {};
    check(host->addModule(std::move(module)).hasValue(), "blocking module");
    std::thread boot([&] { static_cast<void>(host->bootstrap()); });
    if(entry.wait_for(5s) != std::future_status::ready) { std::_Exit(5); }
    std::cerr << "active-bootstrap-delete" << std::endl;
    host.reset();
    blocked.set_value();
    boot.join();
    return 7;
}
}
// Test-only lifetime and process ownership protocol, not a product CLI.
// 中文翻译：仅供寿命及进程所有权测试，不是产品 CLI。
int wmain(int argc, wchar_t** argv)
{
    std::set_terminate([] { std::_Exit(91); });
    try {
        if(argc != 3) { return 2; }
        const std::wstring mode{argv[1]};
        const std::filesystem::path path{argv[2]};
        if(mode == L"hold") { return hold(path); }
        if(mode == L"probe") {
            if(denied(path)) { std::cout << "owned" << std::endl; return 23; }
            std::cout << "acquired" << std::endl; return 0;
        }
        if(mode == L"self-host") { return selfDestroy(true); }
        if(mode == L"self-executor") { return selfDestroy(false); }
        if(mode == L"active-bootstrap") { return activeBootstrap(); }
        return 3;
    } catch(const std::exception& error) { std::cerr << error.what() << std::endl; return 4; }
}
