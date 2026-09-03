if(NOT EXISTS "${SOURCE_ROOT}/cmake/AddressSanitizer.cmake" OR NOT DEFINED EVIDENCE_ROOT)
    message(FATAL_ERROR "ASan configuration source and evidence root are required")
endif()
string(TIMESTAMP stamp "%Y%m%d-%H%M%S")
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef suffix)
set(root "${EVIDENCE_ROOT}/${stamp}-${suffix}")
foreach(mode IN ITEMS production mixed-config non-msvc x86)
    set(testing ON)
    set(msvc TRUE)
    set(pointer_size 8)
    set(config RelWithDebInfo)
    if(mode STREQUAL "production")
        set(testing OFF)
        set(expected "requires LCNC_BUILD_TESTING=ON")
    elseif(mode STREQUAL "mixed-config")
        set(config "Debug;Release")
        set(expected "ASan requires an isolated RelWithDebInfo")
    elseif(mode STREQUAL "non-msvc")
        set(msvc FALSE)
        set(expected "currently requires Windows MSVC x64")
    else()
        set(pointer_size 4)
        set(expected "currently requires Windows MSVC x64")
    endif()
    set(fixture "${root}/${mode}")
    file(MAKE_DIRECTORY "${fixture}")
    # These fixtures test guard logic, not the availability of a real compiler.
    # 中文翻译：这些夹具测试配置防护逻辑，不证明某个实际编译器可用。
    file(WRITE "${fixture}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.25)\nproject(AsanGuard NONE)\n"
        "set(LCNC_ENABLE_ASAN ON)\nset(LCNC_BUILD_TESTING ${testing})\n"
        "set(MSVC ${msvc})\nset(WIN32 TRUE)\nset(CMAKE_SIZEOF_VOID_P ${pointer_size})\n"
        "set(CMAKE_CONFIGURATION_TYPES \"${config}\")\n"
        "include(\"${SOURCE_ROOT}/cmake/AddressSanitizer.cmake\")\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${fixture}" -B "${fixture}/build"
        -G "${GENERATOR}" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
    file(WRITE "${fixture}/configure.log" "${output}\n${error}\nexit=${result}\n")
    if(NOT result MATCHES "^[1-9][0-9]*$" OR NOT "${output}\n${error}" MATCHES "${expected}")
        message(FATAL_ERROR "ASan guard did not reject ${mode} correctly: ${fixture}")
    endif()
    message(STATUS "ASan configuration guard verified: ${mode}")
endforeach()
