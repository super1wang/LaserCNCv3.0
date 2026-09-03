add_library(lasercnc_kernel STATIC)
add_library(LaserCNC::Kernel ALIAS lasercnc_kernel)

target_sources(
    lasercnc_kernel
    PRIVATE
        app_kernel/app_kernel.cpp
        module_runtime/module.cpp
        module_runtime/module_runtime.cpp
        service_registry/service_registry.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
        FILES
            "${PROJECT_SOURCE_DIR}/include/lasercnc/kernel/app_kernel.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/kernel/identifiers.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/kernel/module.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/kernel/module_runtime.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/kernel/service_registry.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/observability/log_service.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/platform/config_serializer.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/platform/persistence_backend.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/platform/hash_service.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/platform/snapshot_store.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/platform/task_executor.hpp"
)

target_link_libraries(lasercnc_kernel PUBLIC LaserCNC::Foundation)
target_compile_features(lasercnc_kernel PUBLIC cxx_std_20)
set_target_properties(
    lasercnc_kernel
    PROPERTIES
        CXX_EXTENSIONS OFF
        DEBUG_POSTFIX d
)

lcnc_enable_warnings(lasercnc_kernel)
