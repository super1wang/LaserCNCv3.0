include_guard(GLOBAL)

if(NOT LCNC_ENABLE_ASAN)
    return()
endif()

# Keep instrumentation out of normal production builds.
# 中文翻译：插桩不得进入常规生产构建。
if(NOT LCNC_BUILD_TESTING)
    message(FATAL_ERROR "LCNC_ENABLE_ASAN requires LCNC_BUILD_TESTING=ON")
endif()
if(NOT MSVC OR NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "LCNC_ENABLE_ASAN currently requires Windows MSVC x64")
endif()
if(CMAKE_CONFIGURATION_TYPES)
    if(NOT "${CMAKE_CONFIGURATION_TYPES}" STREQUAL "RelWithDebInfo")
        message(FATAL_ERROR "ASan requires an isolated RelWithDebInfo configuration; use vs2022-asan")
    endif()
elseif(NOT "${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo")
    message(FATAL_ERROR "ASan requires CMAKE_BUILD_TYPE=RelWithDebInfo")
endif()

get_filename_component(LCNC_ASAN_COMPILER_DIRECTORY "${CMAKE_CXX_COMPILER}" DIRECTORY)
set(LCNC_ASAN_RUNTIME_DLL "${LCNC_ASAN_COMPILER_DIRECTORY}/clang_rt.asan_dynamic-x86_64.dll")
if(NOT EXISTS "${LCNC_ASAN_RUNTIME_DLL}")
    message(FATAL_ERROR "Matching MSVC ASan runtime was not found: ${LCNC_ASAN_RUNTIME_DLL}")
endif()

# Apply before dependencies so compiled adapters, SQLite and test libraries agree.
# 中文翻译：在依赖创建前启用，保证适配器、SQLite 和测试库采用一致插桩。
add_compile_options(/fsanitize=address /Zi)
add_link_options(/INCREMENTAL:NO)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)

# Catch discovery also executes binaries during the build, before CTest starts.
# 中文翻译：Catch 测试发现会在构建阶段执行程序，必须提前提供配套运行库。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/asan-bin")
if(CMAKE_CONFIGURATION_TYPES)
    set(LCNC_ASAN_BINARY_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/RelWithDebInfo")
else()
    set(LCNC_ASAN_BINARY_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()
file(MAKE_DIRECTORY "${LCNC_ASAN_BINARY_DIRECTORY}")
configure_file("${LCNC_ASAN_RUNTIME_DLL}"
    "${LCNC_ASAN_BINARY_DIRECTORY}/clang_rt.asan_dynamic-x86_64.dll" COPYONLY)
message(STATUS "ASan isolated runtime: ${LCNC_ASAN_RUNTIME_DLL}")
