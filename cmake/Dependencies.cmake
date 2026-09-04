include_guard(GLOBAL)
include(FetchContent)

function(lcnc_make_production_dependencies_available)
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
    set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
    if(WIN32)
        # Keep dependency and adapter filename types aligned with native Windows paths.
        # 中文翻译：使依赖与适配器的文件名类型一致使用 Windows 原生宽字符路径。
        set(SPDLOG_WCHAR_FILENAMES ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG 79524ddd08a4ec981b7fea76afd08ee05f83755d GIT_SHALLOW FALSE GIT_SUBMODULES "")
    FetchContent_Declare(jsoncons GIT_REPOSITORY https://github.com/danielaparker/jsoncons.git GIT_TAG bcb44594c50c495ee1e690602cdd71455942ad0e GIT_SHALLOW FALSE GIT_SUBMODULES "" SOURCE_SUBDIR lcnc-no-upstream-build)
    FetchContent_Declare(toml11 GIT_REPOSITORY https://github.com/ToruNiina/toml11.git GIT_TAG be08ba2be2a964edcdb3d3e3ea8d100abc26f286 GIT_SHALLOW FALSE GIT_SUBMODULES "" SOURCE_SUBDIR lcnc-no-upstream-build)
    FetchContent_Declare(bs_thread_pool GIT_REPOSITORY https://github.com/bshoshany/thread-pool.git GIT_TAG bd4533f1f70c2b975cbd5769a60d8eaaea1d2233 GIT_SHALLOW FALSE GIT_SUBMODULES "" SOURCE_SUBDIR lcnc-no-upstream-build)
    FetchContent_Declare(sqlite URL https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip URL_HASH SHA3_256=628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e SOURCE_SUBDIR lcnc-no-upstream-build)

    FetchContent_MakeAvailable(spdlog jsoncons toml11 bs_thread_pool sqlite)

    add_library(lcnc_dependency_spdlog INTERFACE)
    add_library(LaserCNC::Dependency::Spdlog ALIAS lcnc_dependency_spdlog)
    target_link_libraries(lcnc_dependency_spdlog INTERFACE spdlog::spdlog)

    add_library(lcnc_dependency_jsoncons INTERFACE)
    add_library(LaserCNC::Dependency::Jsoncons ALIAS lcnc_dependency_jsoncons)
    target_include_directories(lcnc_dependency_jsoncons INTERFACE "${jsoncons_SOURCE_DIR}/include")

    add_library(lcnc_dependency_toml11 INTERFACE)
    add_library(LaserCNC::Dependency::Toml11 ALIAS lcnc_dependency_toml11)
    target_include_directories(lcnc_dependency_toml11 INTERFACE "${toml11_SOURCE_DIR}/include")

    add_library(lcnc_dependency_bs_thread_pool INTERFACE)
    add_library(LaserCNC::Dependency::BsThreadPool ALIAS lcnc_dependency_bs_thread_pool)
    target_include_directories(lcnc_dependency_bs_thread_pool INTERFACE "${bs_thread_pool_SOURCE_DIR}/include")

    add_library(lcnc_dependency_sqlite STATIC "${sqlite_SOURCE_DIR}/sqlite3.c")
    add_library(LaserCNC::Dependency::SQLite ALIAS lcnc_dependency_sqlite)
    target_include_directories(lcnc_dependency_sqlite PUBLIC "${sqlite_SOURCE_DIR}")
    target_compile_definitions(lcnc_dependency_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_DEFAULT_FOREIGN_KEYS=1 SQLITE_DQS=0 _CRT_SECURE_NO_WARNINGS)
    set_target_properties(lcnc_dependency_sqlite PROPERTIES C_EXTENSIONS OFF)
endfunction()

function(lcnc_make_development_dependencies_available)
    FetchContent_Declare(Catch2 GIT_REPOSITORY https://github.com/catchorg/Catch2.git GIT_TAG 95d8a61b089317bec800c7cc4c64064cbcb3802d GIT_SHALLOW FALSE GIT_SUBMODULES "" EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" PARENT_SCOPE)
endfunction()
