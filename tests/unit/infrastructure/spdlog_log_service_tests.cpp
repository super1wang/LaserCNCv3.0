#include <lasercnc/infrastructure/jsoncons_adapter.hpp>
#include <lasercnc/infrastructure/spdlog_log_service.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace lasercnc::foundation;
using lasercnc::infrastructure::JsonconsAdapter;
using lasercnc::infrastructure::SpdlogLogOptions;
using lasercnc::infrastructure::SpdlogLogService;
using lasercnc::observability::LogLevel;
using lasercnc::observability::LogRecord;

namespace {

std::filesystem::path uniqueLogPath(const char* extension)
{
    static std::atomic<unsigned long long> sequence {0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
           / ("lasercnc-spdlog-" + std::to_string(tick) + '-'
              + std::to_string(sequence.fetch_add(1)) + extension);
}

std::string readText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

void removeFile(const std::filesystem::path& path)
{
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove(path, ignored));
}

} // namespace

TEST_CASE("SpdlogLogService writes human and JSONL records", "[infrastructure][logging]")
{
    const auto humanPath = uniqueLogPath(".log");
    const auto jsonlPath = uniqueLogPath(".jsonl");
    removeFile(humanPath);
    removeFile(jsonlPath);

    {
        SpdlogLogOptions options;
        options.enableConsole = false;
        options.rotatingFilePath = humanPath;
        options.jsonlFilePath = jsonlPath;
        auto created = SpdlogLogService::create(options);
        REQUIRE(created.hasValue());

        LogRecord record;
        record.timestamp = std::chrono::system_clock::time_point {
            std::chrono::milliseconds {1'700'000'000'123LL}};
        record.level = LogLevel::Warning;
        record.module = "kernel.test";
        record.category = "adapter";
        record.message = "structured record";
        record.context.sessionId = "session-1";
        record.context.projectId = "project-2";
        record.context.commandId = "command-3";
        record.context.taskId = "task-4";
        record.context.workflowId = "workflow-5";
        record.context.correlationId = "correlation-6";
        record.context.traceId = "trace-7";
        record.structuredData.emplace("attempt", Value {std::int64_t {2}});

        CHECK(created.value()->write(record).hasValue());
        CHECK(created.value()->flush().hasValue());
    }

    const auto human = readText(humanPath);
    CHECK(human.find("2023-11-14T22:13:20.123Z") != std::string::npos);
    CHECK(human.find("[warning] [kernel.test/adapter] structured record") != std::string::npos);
    CHECK(human.find("traceId=trace-7") != std::string::npos);

    const auto jsonl = readText(jsonlPath);
    REQUIRE_FALSE(jsonl.empty());
    REQUIRE(jsonl.back() == '\n');
    JsonconsAdapter json;
    auto decoded = json.deserialize(jsonl.substr(0, jsonl.size() - 1U));
    REQUIRE(decoded.hasValue());
    const auto* object = decoded.value().getIf<Value::Object>();
    REQUIRE(object != nullptr);
    CHECK(*object->at("timestamp").getIf<std::string>() == "2023-11-14T22:13:20.123Z");
    CHECK(*object->at("level").getIf<std::string>() == "warning");
    CHECK(*object->at("module").getIf<std::string>() == "kernel.test");
    CHECK(*object->at("category").getIf<std::string>() == "adapter");
    CHECK(*object->at("message").getIf<std::string>() == "structured record");
    CHECK(*object->at("sessionId").getIf<std::string>() == "session-1");
    CHECK(*object->at("traceId").getIf<std::string>() == "trace-7");
    const auto* data = object->at("structuredData").getIf<Value::Object>();
    REQUIRE(data != nullptr);
    CHECK(*data->at("attempt").getIf<std::int64_t>() == 2);

    removeFile(humanPath);
    removeFile(jsonlPath);
}

TEST_CASE("SpdlogLogService validates output configuration", "[infrastructure][logging]")
{
    SpdlogLogOptions noOutputs;
    noOutputs.enableConsole = false;
    auto missing = SpdlogLogService::create(noOutputs);
    REQUIRE_FALSE(missing.hasValue());
    CHECK(std::string(missing.error().code.value()) == "Logging.InvalidOptions");

    SpdlogLogOptions sameFile;
    sameFile.enableConsole = false;
    sameFile.rotatingFilePath = "same.log";
    sameFile.jsonlFilePath = "same.log";
    auto conflicting = SpdlogLogService::create(sameFile);
    REQUIRE_FALSE(conflicting.hasValue());
    CHECK(std::string(conflicting.error().code.value()) == "Logging.InvalidOptions");

    SpdlogLogOptions backendFailure;
    backendFailure.enableConsole = false;
    backendFailure.rotatingFilePath = std::filesystem::temp_directory_path();
    auto failed = SpdlogLogService::create(backendFailure);
    REQUIRE_FALSE(failed.hasValue());
    CHECK(std::string(failed.error().code.value()) == "Logging.InitializeFailed");
}
