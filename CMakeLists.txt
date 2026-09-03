cmake_minimum_required(VERSION 3.25)

project(
    LaserCNC
    VERSION 3.0.0
    DESCRIPTION "LaserCNC Application Kernel"
    LANGUAGES C CXX
)

option(LCNC_BUILD_TESTING "构建 LaserCNC 内核测试" ON)
option(LCNC_WARNINGS_AS_ERRORS "将 LaserCNC 自有代码警告视为错误" ON)
option(LCNC_ENABLE_ASAN "启用隔离的 Windows x64 内核 ASan 测试配置" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerWarnings)
include(AddressSanitizer)
include(Dependencies)

lcnc_make_production_dependencies_available()

add_subdirectory(src/foundation)
add_subdirectory(src/kernel)
add_subdirectory(src/state)
add_subdirectory(src/runtime)
add_subdirectory(src/infrastructure)

if(LCNC_BUILD_TESTING)
    include(CTest)
    lcnc_make_development_dependencies_available()
    add_subdirectory(tests)
endif()
