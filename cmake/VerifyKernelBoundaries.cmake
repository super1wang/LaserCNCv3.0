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
