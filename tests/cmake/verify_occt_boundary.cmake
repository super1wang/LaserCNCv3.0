if(NOT DEFINED LCNC_SOURCE_ROOT OR NOT DEFINED LCNC_TEST_BINARY_ROOT)
    message(FATAL_ERROR "架构测试必须提供源目录和独立构建产物目录")
endif()

set(probes "TopoDS_Shape" "TDF_Label" "TDocStd_Document" "Handle(TDocStd_Document)")
set(index 0)
foreach(probe IN LISTS probes)
    math(EXPR index "${index} + 1")
    set(root "${LCNC_TEST_BINARY_ROOT}/occt-boundary-${index}")
    file(MAKE_DIRECTORY "${root}/include/lasercnc/kernel")
    file(WRITE "${root}/include/lasercnc/kernel/app_kernel.hpp" "#pragma once\n")
    file(WRITE "${root}/include/lasercnc/probe.hpp" "${probe}\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DLCNC_SOURCE_ROOT=${root}"
            -P "${LCNC_SOURCE_ROOT}/cmake/VerifyKernelBoundaries.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
    )
    if(result EQUAL 0 OR NOT error MATCHES "禁止类型")
        message(FATAL_ERROR "架构扫描未按预期拒绝 OCCT 公共类型 ${probe}：${output} ${error}")
    endif()
endforeach()
message(STATUS "4 个 OCCT 公共类型泄漏负例全部被拒绝")
