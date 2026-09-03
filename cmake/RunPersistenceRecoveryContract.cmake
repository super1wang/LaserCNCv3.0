if(NOT DEFINED LCNC_CONTRACT_EXECUTABLE OR NOT DEFINED LCNC_STATE_ROOT OR NOT IS_ABSOLUTE "${LCNC_STATE_ROOT}")
    message(FATAL_ERROR "缺少持久化恢复契约参数")
endif()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef nonce)
set(LCNC_STATE_ROOT "${LCNC_STATE_ROOT}/${nonce}")
file(MAKE_DIRECTORY "${LCNC_STATE_ROOT}")

execute_process(
    COMMAND
        "${LCNC_CONTRACT_EXECUTABLE}"
        --mode persistence-seed
        --state-root "${LCNC_STATE_ROOT}"
    RESULT_VARIABLE seed_result
    OUTPUT_VARIABLE seed_output
    ERROR_VARIABLE seed_error
    TIMEOUT 30
)
file(WRITE "${LCNC_STATE_ROOT}/seed-process.log" "exit=${seed_result}\n${seed_output}\n${seed_error}")
if(NOT seed_result EQUAL 0 OR NOT seed_output MATCHES "persistence-seeded")
    message(FATAL_ERROR
        "独立进程持久化播种失败: ${seed_result}\n${seed_output}\n${seed_error}")
endif()

execute_process(
    COMMAND
        "${LCNC_CONTRACT_EXECUTABLE}"
        --mode persistence-recover
        --state-root "${LCNC_STATE_ROOT}"
    RESULT_VARIABLE recovery_result
    OUTPUT_VARIABLE recovery_output
    ERROR_VARIABLE recovery_error
    TIMEOUT 30
)
file(WRITE "${LCNC_STATE_ROOT}/recover-process.log" "exit=${recovery_result}\n${recovery_output}\n${recovery_error}")
if(NOT recovery_result EQUAL 0 OR NOT recovery_output MATCHES "persistence-recovered")
    message(FATAL_ERROR
        "独立进程持久化恢复失败: ${recovery_result}\n${recovery_output}\n${recovery_error}")
endif()

message(STATUS "独立进程持久化恢复契约通过; 证据目录: ${LCNC_STATE_ROOT}")
