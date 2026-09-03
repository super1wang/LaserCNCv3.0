if(NOT DEFINED LCNC_SOURCE_ROOT)
    message(FATAL_ERROR "LCNC_SOURCE_ROOT 未设置")
endif()

file(
    GLOB_RECURSE kernel_public_headers
    LIST_DIRECTORIES FALSE
    "${LCNC_SOURCE_ROOT}/include/lasercnc/*.h"
    "${LCNC_SOURCE_ROOT}/include/lasercnc/*.hpp"
)

set(forbidden_public_api_patterns
    "spdlog::"
    "sqlite3"
    "toml::"
    "jsoncons::"
    "BS::thread_pool"
    "tf::Taskflow"
    "opentelemetry::"
    "QWidget"
    "TopoDS_"
)

foreach(header IN LISTS kernel_public_headers)
    file(READ "${header}" header_content)
    foreach(pattern IN LISTS forbidden_public_api_patterns)
        string(FIND "${header_content}" "${pattern}" match_position)
        if(NOT match_position EQUAL -1)
            message(FATAL_ERROR "Kernel 公共头文件 ${header} 泄漏了禁止类型：${pattern}")
        endif()
    endforeach()
endforeach()

list(LENGTH kernel_public_headers public_header_count)
message(STATUS "Kernel 公共 API 边界检查通过，共检查 ${public_header_count} 个头文件")

file(
    GLOB_RECURSE production_sources
    LIST_DIRECTORIES FALSE
    "${LCNC_SOURCE_ROOT}/include/lasercnc/*.h"
    "${LCNC_SOURCE_ROOT}/include/lasercnc/*.hpp"
    "${LCNC_SOURCE_ROOT}/src/*.c"
    "${LCNC_SOURCE_ROOT}/src/*.cc"
    "${LCNC_SOURCE_ROOT}/src/*.cpp"
)

function(lcnc_require_pattern_in_directory pattern allowed_directory)
    foreach(source_file IN LISTS production_sources)
        file(READ "${source_file}" source_content)
        string(FIND "${source_content}" "${pattern}" match_position)
        if(NOT match_position EQUAL -1)
            file(TO_CMAKE_PATH "${source_file}" normalized_source_file)
            string(FIND "${normalized_source_file}" "${allowed_directory}" allowed_position)
            if(allowed_position EQUAL -1)
                message(FATAL_ERROR
                    "第三方边界违规：${pattern} 只能出现在 ${allowed_directory}，实际位于 ${source_file}")
            endif()
        endif()
    endforeach()
endfunction()

lcnc_require_pattern_in_directory("spdlog::" "/src/infrastructure/logging/spdlog/")
lcnc_require_pattern_in_directory("#include <spdlog/" "/src/infrastructure/logging/spdlog/")
lcnc_require_pattern_in_directory("sqlite3" "/src/infrastructure/persistence/sqlite/")
lcnc_require_pattern_in_directory("jsoncons::" "/src/infrastructure/serialization/jsoncons/")
lcnc_require_pattern_in_directory("#include <jsoncons/" "/src/infrastructure/serialization/jsoncons/")
lcnc_require_pattern_in_directory("toml::" "/src/infrastructure/config/toml11/")
lcnc_require_pattern_in_directory("#include <toml.hpp>" "/src/infrastructure/config/toml11/")
lcnc_require_pattern_in_directory("BS::thread_pool" "/src/infrastructure/execution/bs_thread_pool/")
lcnc_require_pattern_in_directory("#include <BS_thread_pool.hpp>" "/src/infrastructure/execution/bs_thread_pool/")
lcnc_require_pattern_in_directory("BCrypt" "/src/infrastructure/hash/windows/")
lcnc_require_pattern_in_directory("#include <bcrypt.h>" "/src/infrastructure/hash/windows/")
lcnc_require_pattern_in_directory("MoveFileExW" "/src/infrastructure/persistence/filesystem/windows/")
lcnc_require_pattern_in_directory("CreateFileW" "/src/infrastructure/persistence/filesystem/windows/")

list(LENGTH production_sources production_source_count)
message(STATUS "第三方实现目录隔离检查通过，共检查 ${production_source_count} 个生产源文件")
