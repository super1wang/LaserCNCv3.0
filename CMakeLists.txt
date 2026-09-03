cmake_minimum_required(VERSION 3.25)

project(
    LaserCNC
    VERSION 3.0.0
    DESCRIPTION "LaserCNC Application Kernel"
    LANGUAGES C CXX
)

option(LCNC_BUILD_TESTING "构建 LaserCNC 内核测试" ON)
option(LCNC_WARNINGS_AS_ERRORS "将 LaserCNC 自有代码警告视为错误" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerWarnings)
include(Dependencies)

lcnc_make_production_dependencies_available()

add_subdirectory(src/foundation)
add_subdirectory(src/kernel)

if(LCNC_BUILD_TESTING)
    include(CTest)
    lcnc_make_development_dependencies_available()
    add_subdirectory(tests)
endif()
