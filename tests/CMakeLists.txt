add_executable(
    lasercnc_foundation_tests
    unit/foundation/strong_id_tests.cpp
    unit/foundation/value_tests.cpp
    unit/foundation/result_error_tests.cpp
    unit/foundation/schema_tests.cpp
)

target_link_libraries(
    lasercnc_foundation_tests
    PRIVATE
        LaserCNC::Foundation
        Catch2::Catch2WithMain
)

target_compile_features(lasercnc_foundation_tests PRIVATE cxx_std_20)
set_target_properties(lasercnc_foundation_tests PROPERTIES CXX_EXTENSIONS OFF)
lcnc_enable_warnings(lasercnc_foundation_tests)

include(Catch)
catch_discover_tests(lasercnc_foundation_tests)

# Allocation replacement is isolated to this test process, never a production target.
# 中文翻译：分配替换仅存在于此测试进程，不进入生产目标或其他测试进程。
add_executable(lasercnc_trace_allocation_probe integration/trace_allocation_probe.cpp)
target_link_libraries(lasercnc_trace_allocation_probe PRIVATE LaserCNC::Kernel)
target_compile_features(lasercnc_trace_allocation_probe PRIVATE cxx_std_20)
set_target_properties(lasercnc_trace_allocation_probe PROPERTIES CXX_EXTENSIONS OFF)
lcnc_enable_warnings(lasercnc_trace_allocation_probe)
add_test(NAME integration.trace_allocation_atomicity
    COMMAND "${CMAKE_COMMAND}" "-DLCNC_EXECUTABLE=$<TARGET_FILE:lasercnc_trace_allocation_probe>"
        "-DLCNC_EXPECTED_OUTPUT=trace-start-allocation-verified" "-DLCNC_TIMEOUT=30"
        -P "${PROJECT_SOURCE_DIR}/cmake/RunVerifiedProgram.cmake")
set_tests_properties(integration.trace_allocation_atomicity PROPERTIES TIMEOUT 40)

# Test-only process probe; no product command-line contract is introduced.
# 中文翻译：仅供测试的进程探针，不新增产品命令行契约。
add_executable(lasercnc_lifecycle_catalog_probe integration/lifecycle_catalog_probe.cpp)
target_link_libraries(lasercnc_lifecycle_catalog_probe PRIVATE LaserCNC::Infrastructure)
target_compile_features(lasercnc_lifecycle_catalog_probe PRIVATE cxx_std_20)
set_target_properties(lasercnc_lifecycle_catalog_probe PROPERTIES CXX_EXTENSIONS OFF)
lcnc_enable_warnings(lasercnc_lifecycle_catalog_probe)
add_test(NAME integration.lifecycle_catalog_epoch_restart
    COMMAND "${CMAKE_COMMAND}" "-DPROBE=$<TARGET_FILE:lasercnc_lifecycle_catalog_probe>"
        "-DEVIDENCE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/lifecycle-catalog-process-runs"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_lifecycle_catalog_epoch.cmake")
set_tests_properties(integration.lifecycle_catalog_epoch_restart PROPERTIES TIMEOUT 70)

add_executable(lasercnc_close_snapshot_probe integration/close_snapshot_identity_probe.cpp)
target_link_libraries(lasercnc_close_snapshot_probe PRIVATE LaserCNC::Infrastructure)
target_compile_features(lasercnc_close_snapshot_probe PRIVATE cxx_std_20)
set_target_properties(lasercnc_close_snapshot_probe PROPERTIES CXX_EXTENSIONS OFF)
lcnc_enable_warnings(lasercnc_close_snapshot_probe)
add_test(NAME integration.close_snapshot_identity_restart
    COMMAND "${CMAKE_COMMAND}" "-DPROBE=$<TARGET_FILE:lasercnc_close_snapshot_probe>"
        "-DEVIDENCE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/close-snapshot-process-runs"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_close_snapshot_identity.cmake")
set_tests_properties(integration.close_snapshot_identity_restart PROPERTIES TIMEOUT 190 RUN_SERIAL TRUE)

add_test(NAME architecture.asan_configuration_guards
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_ROOT=${PROJECT_SOURCE_DIR}"
        "-DEVIDENCE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/asan-config-probes"
        "-DGENERATOR=${CMAKE_GENERATOR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_asan_configuration.cmake")
set_tests_properties(architecture.asan_configuration_guards PROPERTIES TIMEOUT 60)

add_test(NAME architecture.verified_program_exit_guards
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_ROOT=${PROJECT_SOURCE_DIR}"
        "-DEVIDENCE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/verified-program-probes"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_program_exit_guard.cmake")
set_tests_properties(architecture.verified_program_exit_guards PROPERTIES TIMEOUT 45)

if(LCNC_ENABLE_ASAN)
    add_executable(lasercnc_asan_probe integration/kernel_asan_probe.cpp)
    target_compile_features(lasercnc_asan_probe PRIVATE cxx_std_20)
    set_target_properties(lasercnc_asan_probe PROPERTIES CXX_EXTENSIONS OFF)
    lcnc_enable_warnings(lasercnc_asan_probe)
    foreach(mode IN ITEMS healthy heap-buffer-overflow heap-use-after-free)
        add_test(NAME integration.kernel_asan_probe_${mode}
            COMMAND "${CMAKE_COMMAND}" "-DPROBE=$<TARGET_FILE:lasercnc_asan_probe>"
                "-DMODE=${mode}" "-DEVIDENCE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/asan-probes"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_asan_probe.cmake")
        set_tests_properties(integration.kernel_asan_probe_${mode} PROPERTIES TIMEOUT 45)
    endforeach()
endif()

# Benchmark harness is test-only; it is not a product command-line host.
# 中文翻译：基准程序仅用于测试，不是产品命令行宿主。
if(WIN32)
    find_program(LCNC_WINDOWS_POWERSHELL NAMES powershell.exe REQUIRED)
    add_executable(lasercnc_kernel_drain_probe integration/kernel_drain_probe.cpp)
    target_link_libraries(lasercnc_kernel_drain_probe PRIVATE LaserCNC::Infrastructure)
    target_include_directories(lasercnc_kernel_drain_probe PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
    target_compile_features(lasercnc_kernel_drain_probe PRIVATE cxx_std_20)
    set_target_properties(lasercnc_kernel_drain_probe PROPERTIES CXX_EXTENSIONS OFF)
    lcnc_enable_warnings(lasercnc_kernel_drain_probe)
    add_test(NAME integration.kernel_final_drain_process
        COMMAND "${CMAKE_COMMAND}"
            "-DLCNC_EXECUTABLE=${LCNC_WINDOWS_POWERSHELL}"
            "-DLCNC_ARGUMENTS=-NoProfile;-ExecutionPolicy;Bypass;-File;${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_kernel_drain.ps1;-Executable;$<TARGET_FILE:lasercnc_kernel_drain_probe>;-EvidenceRoot;${CMAKE_CURRENT_BINARY_DIR}/final-drain-process-runs"
            "-DLCNC_EXPECTED_OUTPUT=kernel-final-drain-process-verified" "-DLCNC_TIMEOUT=80"
            -P "${PROJECT_SOURCE_DIR}/cmake/RunVerifiedProgram.cmake")
    set_tests_properties(integration.kernel_final_drain_process PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
    add_executable(lasercnc_persistence_session_probe integration/kernel_persistence_session_probe.cpp)
    target_link_libraries(lasercnc_persistence_session_probe PRIVATE LaserCNC::Infrastructure)
    target_compile_features(lasercnc_persistence_session_probe PRIVATE cxx_std_20)
    set_target_properties(lasercnc_persistence_session_probe PROPERTIES CXX_EXTENSIONS OFF)
    lcnc_enable_warnings(lasercnc_persistence_session_probe)
    add_test(NAME integration.persistence_host_session_process
        COMMAND "${CMAKE_COMMAND}"
            "-DLCNC_EXECUTABLE=${LCNC_WINDOWS_POWERSHELL}"
            "-DLCNC_ARGUMENTS=-NoProfile;-ExecutionPolicy;Bypass;-File;${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_persistence_session.ps1;-Executable;$<TARGET_FILE:lasercnc_persistence_session_probe>;-EvidenceRoot;${CMAKE_CURRENT_BINARY_DIR}/host-session-process-runs"
            "-DLCNC_EXPECTED_OUTPUT=host-session-process-verified" "-DLCNC_TIMEOUT=35"
            -P "${PROJECT_SOURCE_DIR}/cmake/RunVerifiedProgram.cmake")
    set_tests_properties(integration.persistence_host_session_process PROPERTIES
        TIMEOUT 45 RUN_SERIAL TRUE)
    add_test(NAME integration.persistence_host_initialization_process
        COMMAND "${CMAKE_COMMAND}"
            "-DLCNC_EXECUTABLE=${LCNC_WINDOWS_POWERSHELL}"
            "-DLCNC_ARGUMENTS=-NoProfile;-ExecutionPolicy;Bypass;-File;${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_persistence_session.ps1;-Executable;$<TARGET_FILE:lasercnc_persistence_session_probe>;-EvidenceRoot;${CMAKE_CURRENT_BINARY_DIR}/host-initialization-process-runs;-Service"
            "-DLCNC_EXPECTED_OUTPUT=host-session-process-verified" "-DLCNC_TIMEOUT=35"
            -P "${PROJECT_SOURCE_DIR}/cmake/RunVerifiedProgram.cmake")
    set_tests_properties(integration.persistence_host_initialization_process PROPERTIES
        TIMEOUT 45 RUN_SERIAL TRUE)

    add_executable(lasercnc_kernel_benchmark benchmark/kernel_benchmark.cpp)
    target_link_libraries(lasercnc_kernel_benchmark PRIVATE LaserCNC::Infrastructure Psapi)
    target_include_directories(lasercnc_kernel_benchmark PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
    target_compile_features(lasercnc_kernel_benchmark PRIVATE cxx_std_20)
    set_target_properties(lasercnc_kernel_benchmark PROPERTIES CXX_EXTENSIONS OFF)
    target_compile_definitions(lasercnc_kernel_benchmark PRIVATE
        LCNC_BENCH_ROOT="${CMAKE_CURRENT_BINARY_DIR}/benchmark-runs"
        LCNC_BENCH_CONFIG="$<CONFIG>"
        LCNC_BENCH_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")
    lcnc_enable_warnings(lasercnc_kernel_benchmark)
    foreach(family IN ITEMS component gateway journal lifecycle)
        foreach(storage IN ITEMS memory sqlite)
            if(family STREQUAL "journal" AND storage STREQUAL "memory")
                continue()
            endif()
            add_test(NAME integration.kernel_benchmark_smoke_${family}_${storage}
                COMMAND "${CMAKE_COMMAND}"
                    "-DLCNC_EXECUTABLE=$<TARGET_FILE:lasercnc_kernel_benchmark>"
                    "-DLCNC_ARGUMENTS=--objects;10;--samples;2;--warmup;1;--cycles;3;--family;${family};--storage;${storage}"
                    "-DLCNC_EXPECTED_OUTPUT=benchmark-verified" "-DLCNC_TIMEOUT=110"
                    -P "${PROJECT_SOURCE_DIR}/cmake/RunVerifiedProgram.cmake")
            set_tests_properties(integration.kernel_benchmark_smoke_${family}_${storage} PROPERTIES
                TIMEOUT 120 RUN_SERIAL TRUE)
        endforeach()
    endforeach()
endif()

add_executable(
    lasercnc_kernel_tests
    unit/kernel/document_state_tests.cpp
    unit/kernel/object_type_registry_tests.cpp
    unit/runtime/object_record_codec_tests.cpp
    unit/kernel/service_registry_tests.cpp
    unit/kernel/transaction_tests.cpp
    unit/kernel/runtime_registry_tests.cpp
    unit/kernel/event_bus_tests.cpp
    unit/kernel/command_query_runtime_tests.cpp
    unit/kernel/module_runtime_tests.cpp
    unit/kernel/infrastructure_ports_tests.cpp
    unit/runtime/observability_tests.cpp
    unit/runtime/workflow_registry_tests.cpp
)

target_link_libraries(
    lasercnc_kernel_tests
    PRIVATE
        LaserCNC::Kernel
        Catch2::Catch2WithMain
)
target_include_directories(lasercnc_kernel_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")

target_compile_features(lasercnc_kernel_tests PRIVATE cxx_std_20)
set_target_properties(lasercnc_kernel_tests PROPERTIES CXX_EXTENSIONS OFF)
lcnc_enable_warnings(lasercnc_kernel_tests)

catch_discover_tests(lasercnc_kernel_tests PROPERTIES TIMEOUT 120)

add_executable(
    lasercnc_infrastructure_tests
    unit/infrastructure/bs_thread_pool_executor_tests.cpp
    unit/infrastructure/jsoncons_adapter_tests.cpp
    unit/infrastructure/sha256_hash_service_tests.cpp
    unit/infrastructure/filesystem_snapshot_store_tests.cpp
    unit/infrastructure/snapshot_storage_key_tests.cpp
    unit/infrastructure/snapshot_create_probe.cpp
    unit/infrastructure/filesystem_asset_store_tests.cpp
    unit/infrastructure/file_path_admission_tests.cpp
    unit/infrastructure/logging_path_identity_tests.cpp
    unit/infrastructure/spdlog_log_service_tests.cpp
    unit/infrastructure/sqlite_persistence_backend_tests.cpp
    unit/infrastructure/toml_config_adapter_tests.cpp
    unit/runtime/task_runtime_tests.cpp
    unit/runtime/persistence_service_tests.cpp
    unit/runtime/project_persistence_tests.cpp
    unit/runtime/project_task_admission_tests.cpp
    unit/runtime/project_runtime_tests.cpp
    unit/runtime/document_recovery_admission_tests.cpp
    unit/runtime/lifecycle_catalog_tests.cpp
    unit/runtime/close_snapshot_identity_tests.cpp
    unit/runtime/snapshot_ordering_tests.cpp
    unit/runtime/kernel_shutdown_tests.cpp
    unit/runtime/history_runtime_tests.cpp
    unit/runtime/asset_state_tests.cpp
    unit/runtime/kernel_concurrency_tests.cpp
)
target_link_libraries(
    lasercnc_infrastructure_tests
    PRIVATE
        LaserCNC::Infrastructure
        Catch2::Catch2WithMain
)
target_include_directories(lasercnc_infrastructure_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
target_compile_definitions(lasercnc_infrastructure_tests PRIVATE
    LCNC_STRESS_TEST_ROOT="${CMAKE_CURRENT_BINARY_DIR}/stress-contract-runs")
target_compile_features(lasercnc_infrastructure_tests PRIVATE cxx_std_20)
set_target_properties(lasercnc_infrastructure_tests PROPERTIES CXX_EXTENSIONS OFF)
lcnc_enable_warnings(lasercnc_infrastructure_tests)
catch_discover_tests(lasercnc_infrastructure_tests PROPERTIES TIMEOUT 300)

add_test(
    NAME architecture.kernel_public_api_boundary
    COMMAND
        "${CMAKE_COMMAND}"
        "-DLCNC_SOURCE_ROOT=${PROJECT_SOURCE_DIR}"
        -P "${PROJECT_SOURCE_DIR}/cmake/VerifyKernelBoundaries.cmake"
)

add_test(
    NAME architecture.occt_boundary_negative_probes
    COMMAND "${CMAKE_COMMAND}"
        "-DLCNC_SOURCE_ROOT=${PROJECT_SOURCE_DIR}"
        "-DLCNC_TEST_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/architecture-probes"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_occt_boundary.cmake"
)

add_test(
    NAME architecture.host_state_boundary_negative_probes
    COMMAND "${CMAKE_COMMAND}"
        "-DLCNC_SOURCE_ROOT=${PROJECT_SOURCE_DIR}"
        "-DLCNC_TEST_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/architecture-probes"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_host_state_boundary.cmake"
)
set_tests_properties(architecture.host_state_boundary_negative_probes PROPERTIES TIMEOUT 40)

add_test(NAME architecture.host_document_compile_boundary
    COMMAND "${CMAKE_COMMAND}"
        "-DLCNC_SOURCE_ROOT=${PROJECT_SOURCE_DIR}"
        "-DLCNC_TEST_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/architecture-probes"
        "-DLCNC_GENERATOR=${CMAKE_GENERATOR}"
        "-DLCNC_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_host_document_compile.cmake")
set_tests_properties(architecture.host_document_compile_boundary PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

add_executable(
    lasercnc_kernel_headless_contract
    integration/kernel_headless_contract.cpp
    integration/kernel_crash_contract.cpp
    integration/kernel_file_crash_probe.cpp
)
target_link_libraries(
    lasercnc_kernel_headless_contract
    PRIVATE LaserCNC::Infrastructure
)
target_include_directories(lasercnc_kernel_headless_contract PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
target_compile_features(lasercnc_kernel_headless_contract PRIVATE cxx_std_20)
set_target_properties(
    lasercnc_kernel_headless_contract
    PROPERTIES CXX_EXTENSIONS OFF
)
lcnc_enable_warnings(lasercnc_kernel_headless_contract)

foreach(scenario IN ITEMS command-staged journal-inserted outcome-written transaction-before-commit
    transaction-committed command-returned undo-inserted undo-committed redo-inserted redo-committed
    task-handler workflow-handler workflow-committed effect-safe effect-idempotent effect-reconcile effect-never
    workflow-effect-reconcile workflow-effect-never
    asset-before-write asset-before-rename asset-after-rename asset-published
    asset-reference-inserted asset-reference-committed asset-missing-committed asset-corrupt-committed)
    add_test(NAME integration.kernel_crash_${scenario}
        COMMAND "${CMAKE_COMMAND}"
            "-DLCNC_CONTRACT_EXECUTABLE=$<TARGET_FILE:lasercnc_kernel_headless_contract>"
            "-DLCNC_TEST_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
            "-DLCNC_CRASH_SCENARIO=${scenario}"
            -P "${PROJECT_SOURCE_DIR}/cmake/RunKernelCrashContract.cmake")
    set_tests_properties(integration.kernel_crash_${scenario} PROPERTIES
        TIMEOUT 100 RUN_SERIAL TRUE)
endforeach()

add_test(
    NAME integration.kernel_headless_cli_roundtrip
    COMMAND "${CMAKE_COMMAND}"
        "-DLCNC_EXECUTABLE=$<TARGET_FILE:lasercnc_kernel_headless_contract>"
        "-DLCNC_ARGUMENTS=--mode;roundtrip" "-DLCNC_EXPECTED_OUTPUT=verified"
        -P "${PROJECT_SOURCE_DIR}/cmake/RunVerifiedProgram.cmake"
)
set_tests_properties(
    integration.kernel_headless_cli_roundtrip
    PROPERTIES TIMEOUT 40
)

add_test(
    NAME integration.kernel_headless_task_roundtrip
    COMMAND "${CMAKE_COMMAND}"
        "-DLCNC_EXECUTABLE=$<TARGET_FILE:lasercnc_kernel_headless_contract>"
        "-DLCNC_ARGUMENTS=--mode;task-roundtrip" "-DLCNC_EXPECTED_OUTPUT=task-verified"
        -P "${PROJECT_SOURCE_DIR}/cmake/RunVerifiedProgram.cmake"
)
set_tests_properties(
    integration.kernel_headless_task_roundtrip
    PROPERTIES TIMEOUT 40
)

add_test(
    NAME integration.kernel_persistence_process_recovery
    COMMAND
        "${CMAKE_COMMAND}"
        "-DLCNC_CONTRACT_EXECUTABLE=$<TARGET_FILE:lasercnc_kernel_headless_contract>"
        "-DLCNC_STATE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/persistence-process-contract"
        -P "${PROJECT_SOURCE_DIR}/cmake/RunPersistenceRecoveryContract.cmake"
)
set_tests_properties(
    integration.kernel_persistence_process_recovery
    PROPERTIES
        TIMEOUT 70
        RUN_SERIAL TRUE
)

add_test(
    NAME integration.kernel_workflow_process_recovery
    COMMAND
        "${CMAKE_COMMAND}"
        "-DLCNC_CONTRACT_EXECUTABLE=$<TARGET_FILE:lasercnc_kernel_headless_contract>"
        "-DLCNC_STATE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/workflow-process-contract"
        -P "${PROJECT_SOURCE_DIR}/cmake/RunWorkflowRecoveryContract.cmake"
)
set_tests_properties(
    integration.kernel_workflow_process_recovery
    PROPERTIES
        TIMEOUT 70
        RUN_SERIAL TRUE
)
