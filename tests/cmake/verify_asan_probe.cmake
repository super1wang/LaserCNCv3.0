if(NOT DEFINED PROBE OR NOT EXISTS "${PROBE}" OR NOT DEFINED EVIDENCE_ROOT)
    message(FATAL_ERROR "ASan probe executable and evidence root are required")
endif()
if(NOT MODE MATCHES "^(healthy|heap-buffer-overflow|heap-use-after-free)$")
    message(FATAL_ERROR "Unknown ASan probe mode")
endif()
string(TIMESTAMP stamp "%Y%m%d-%H%M%S")
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef suffix)
set(evidence "${EVIDENCE_ROOT}/${stamp}-${MODE}-${suffix}")
file(MAKE_DIRECTORY "${evidence}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env --unset=ASAN_WIN_CONTINUE_ON_INTERCEPTION_FAILURE
        "ASAN_OPTIONS=alloc_dealloc_mismatch=1:abort_on_error=1" "${PROBE}" "${MODE}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
file(WRITE "${evidence}/stdout.log" "${output}")
file(WRITE "${evidence}/stderr.log" "${error}")
file(WRITE "${evidence}/result.txt" "${result}\n")
if(MODE STREQUAL "healthy")
    if(NOT result STREQUAL "0" OR NOT output MATCHES "asan-probe-healthy"
       OR error MATCHES "AddressSanitizer")
        message(FATAL_ERROR "ASan healthy control failed: ${result}; evidence: ${evidence}")
    endif()
else()
    if(NOT result MATCHES "^-?[1-9][0-9]*$"
       OR NOT error MATCHES "ERROR: AddressSanitizer: ${MODE}")
        message(FATAL_ERROR "ASan did not detect ${MODE}: ${result}; evidence: ${evidence}")
    endif()
endif()
message(STATUS "ASan probe verified: ${MODE}; evidence: ${evidence}")
