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

add_executable(
    lasercnc_kernel_tests
    unit/kernel/document_state_tests.cpp
    unit/kernel/service_registry_tests.cpp
    unit/kernel/transaction_tests.cpp
    unit/kernel/runtime_registry_tests.cpp
    unit/kernel/event_bus_tests.cpp
    unit/kernel/command_query_runtime_tests.cpp
    unit/kernel/module_runtime_tests.cpp
    unit/kernel/infrastructure_ports_tests.cpp
    unit/runtime/observability_tests.cpp
)

target_link_libraries(
    lasercnc_kernel_tests
    PRIVATE
        LaserCNC::Kernel
        Catch2::Catch2WithMain
)

target_compile_features(lasercnc_kernel_tests PRIVATE cxx_std_20)
set_target_properties(lasercnc_kernel_tests PROPERTIES CXX_EXTENSIONS OFF)
lcnc_enable_warnings(lasercnc_kernel_tests)

catch_discover_tests(lasercnc_kernel_tests)

add_executable(
    lasercnc_infrastructure_tests
    unit/infrastructure/bs_thread_pool_executor_tests.cpp
    unit/infrastructure/jsoncons_adapter_tests.cpp
    unit/infrastructure/spdlog_log_service_tests.cpp
    unit/infrastructure/sqlite_persistence_backend_tests.cpp
    unit/infrastructure/toml_config_adapter_tests.cpp
    unit/runtime/task_runtime_tests.cpp
)
target_link_libraries(
    lasercnc_infrastructure_tests
    PRIVATE
        LaserCNC::Infrastructure
        Catch2::Catch2WithMain
)
target_compile_features(lasercnc_infrastructure_tests PRIVATE cxx_std_20)
set_target_properties(lasercnc_infrastructure_tests PROPERTIES CXX_EXTENSIONS OFF)
lcnc_enable_warnings(lasercnc_infrastructure_tests)
catch_discover_tests(lasercnc_infrastructure_tests)

add_test(
    NAME architecture.kernel_public_api_boundary
    COMMAND
        "${CMAKE_COMMAND}"
        "-DLCNC_SOURCE_ROOT=${PROJECT_SOURCE_DIR}"
        -P "${PROJECT_SOURCE_DIR}/cmake/VerifyKernelBoundaries.cmake"
)

add_executable(
    lasercnc_kernel_headless_contract
    integration/kernel_headless_contract.cpp
)
target_link_libraries(
    lasercnc_kernel_headless_contract
    PRIVATE LaserCNC::Infrastructure
)
target_compile_features(lasercnc_kernel_headless_contract PRIVATE cxx_std_20)
set_target_properties(
    lasercnc_kernel_headless_contract
    PROPERTIES CXX_EXTENSIONS OFF
)
lcnc_enable_warnings(lasercnc_kernel_headless_contract)

add_test(
    NAME integration.kernel_headless_cli_roundtrip
    COMMAND lasercnc_kernel_headless_contract --mode roundtrip
)
set_tests_properties(
    integration.kernel_headless_cli_roundtrip
    PROPERTIES PASS_REGULAR_EXPRESSION "verified"
)

add_test(
    NAME integration.kernel_headless_task_roundtrip
    COMMAND lasercnc_kernel_headless_contract --mode task-roundtrip
)
set_tests_properties(
    integration.kernel_headless_task_roundtrip
    PROPERTIES PASS_REGULAR_EXPRESSION "task-verified"
)
