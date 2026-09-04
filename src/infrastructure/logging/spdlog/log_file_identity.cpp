#include "log_file_identity.hpp"
#include "../../persistence/filesystem/windows/log_file_identity.hpp"
#include <lasercnc/foundation/error.hpp>
#include <spdlog/sinks/rotating_file_sink.h>
#include <set>
#include <utility>
#include <vector>

namespace lasercnc::infrastructure::detail {
namespace {
foundation::Result<void> conflict(const char* reason)
{
    return foundation::Result<void>::failure(foundation::makeError("Logging.InvalidOptions",
        foundation::ErrorCategory::Validation, "The logging adapter options are invalid",
        foundation::Value{foundation::Value::Object{{"reason", foundation::Value{reason}}}}));
}
} // namespace

foundation::Result<void> validateLogFileIdentities(const SpdlogLogOptions& options)
{
    std::vector<std::filesystem::path> bases;
    if(options.rotatingFilePath) { bases.push_back(*options.rotatingFilePath); }
    if(options.jsonlFilePath) { bases.push_back(*options.jsonlFilePath); }
    if(bases.empty()) { return foundation::Result<void>::success(); }
    if(options.rotatingFileCount > spdlog::sinks::rotating_file_sink_mt::MaxFiles) {
        return conflict("Retained log file count exceeds the supported rotating sink limit");
    }
    std::set<TargetName, NameLess> names;
    std::set<FileIdentity> files;
    for(const auto& base : bases) {
        for(std::size_t index = 0U; index <= options.rotatingFileCount; ++index) {
            // Use the backend's exact naming function, including dotfiles and multi-dot extensions.
            // 中文翻译：复用后端真实轮转命名函数，覆盖点文件和多点扩展名。
            auto target = inspectLogFileTarget(std::filesystem::path{
                spdlog::sinks::rotating_file_sink_mt::calc_filename(base.native(), index)});
            if(!names.insert(std::move(target.name)).second) {
                return conflict("Log outputs or rotation targets resolve to the same file name");
            }
            if(target.existing && !files.insert(*target.existing).second) {
                return conflict("Log outputs or rotation targets share an existing physical file");
            }
        }
    }
    return foundation::Result<void>::success();
}
} // namespace lasercnc::infrastructure::detail
