cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED LCNC_CONTRACT_EXECUTABLE OR NOT DEFINED LCNC_TEST_BINARY_ROOT
   OR NOT DEFINED LCNC_CRASH_SCENARIO OR NOT IS_ABSOLUTE "${LCNC_TEST_BINARY_ROOT}")
    message(FATAL_ERROR "必须提供契约程序、绝对测试构建目录和崩溃场景")
endif()
set(scenarios command-staged journal-inserted outcome-written transaction-before-commit
    transaction-committed command-returned undo-inserted undo-committed redo-inserted redo-committed
    task-handler workflow-handler workflow-committed effect-safe effect-idempotent effect-reconcile effect-never
    workflow-effect-reconcile workflow-effect-never)
if(NOT LCNC_CRASH_SCENARIO IN_LIST scenarios)
    message(FATAL_ERROR "未知崩溃场景")
endif()
if(LCNC_CRASH_SCENARIO STREQUAL "command-staged" OR LCNC_CRASH_SCENARIO STREQUAL "workflow-handler")
    set(expected_point handler-staged)
elseif(LCNC_CRASH_SCENARIO STREQUAL "task-handler")
    set(expected_point task-handler)
elseif(LCNC_CRASH_SCENARIO MATCHES "effect-")
    set(expected_point effect-handler)
elseif(LCNC_CRASH_SCENARIO MATCHES "inserted$")
    set(expected_point journal-inserted)
elseif(LCNC_CRASH_SCENARIO STREQUAL "outcome-written")
    set(expected_point outcome-written)
elseif(LCNC_CRASH_SCENARIO STREQUAL "transaction-before-commit")
    set(expected_point before-commit)
elseif(LCNC_CRASH_SCENARIO MATCHES "committed$")
    set(expected_point after-commit-before-memory)
else()
    set(expected_point command-returned)
endif()
# Every invocation gets a new child directory; never recursively delete caller paths.
# 中文翻译：每轮创建独立子目录，不递归删除调用方传入的目录，保留失败证据。
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef nonce)
set(state_root "${LCNC_TEST_BINARY_ROOT}/crash-contract-runs/${LCNC_CRASH_SCENARIO}-${nonce}")
file(MAKE_DIRECTORY "${state_root}")
execute_process(COMMAND "${LCNC_CONTRACT_EXECUTABLE}" --mode crash-seed --state-root "${state_root}"
    --scenario "${LCNC_CRASH_SCENARIO}" RESULT_VARIABLE seed_result OUTPUT_VARIABLE seed_output
    ERROR_VARIABLE seed_error TIMEOUT 30)
file(WRITE "${state_root}/seed-process.log" "exit=${seed_result}\n${seed_output}\n${seed_error}")
if(NOT seed_result STREQUAL "86" OR NOT seed_output MATCHES "crash-point:${LCNC_CRASH_SCENARIO}:${expected_point}")
    message(FATAL_ERROR "子进程未到达指定崩溃点: ${seed_result} ${seed_output} ${seed_error}; 证据目录: ${state_root}")
endif()
foreach(mode IN ITEMS recover audit)
    execute_process(COMMAND "${LCNC_CONTRACT_EXECUTABLE}" --mode "crash-${mode}" --state-root "${state_root}"
        --scenario "${LCNC_CRASH_SCENARIO}" RESULT_VARIABLE result OUTPUT_VARIABLE output
        ERROR_VARIABLE error TIMEOUT 30)
    file(WRITE "${state_root}/${mode}-process.log" "exit=${result}\n${output}\n${error}")
    if(NOT result EQUAL 0 OR NOT output MATCHES "crash-${mode}ed:${LCNC_CRASH_SCENARIO}")
        message(FATAL_ERROR "独立进程 ${mode} 失败: ${result} ${output} ${error}; 证据目录: ${state_root}")
    endif()
endforeach()
message(STATUS "独立进程崩溃契约通过: ${LCNC_CRASH_SCENARIO}; 证据目录: ${state_root}")
