add_library(lasercnc_infrastructure STATIC)
add_library(LaserCNC::Infrastructure ALIAS lasercnc_infrastructure)

target_sources(
    lasercnc_infrastructure
    PRIVATE
        config/toml11/toml_config_adapter.cpp
        execution/bs_thread_pool/bs_thread_pool_executor.cpp
        logging/spdlog/spdlog_log_service.cpp
        persistence/sqlite/sqlite_persistence_backend.cpp
        serialization/jsoncons/jsoncons_adapter.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
        FILES
            "${PROJECT_SOURCE_DIR}/include/lasercnc/infrastructure/jsoncons_adapter.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/infrastructure/bs_thread_pool_executor.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/infrastructure/spdlog_log_service.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/infrastructure/sqlite_persistence_backend.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/infrastructure/toml_config_adapter.hpp"
)

target_link_libraries(
    lasercnc_infrastructure
    PUBLIC LaserCNC::Kernel
    PRIVATE
        LaserCNC::Dependency::Jsoncons
        LaserCNC::Dependency::Toml11
        LaserCNC::Dependency::BsThreadPool
        LaserCNC::Dependency::Spdlog
        LaserCNC::Dependency::SQLite
)
target_compile_features(lasercnc_infrastructure PUBLIC cxx_std_20)
set_target_properties(lasercnc_infrastructure PROPERTIES CXX_EXTENSIONS OFF DEBUG_POSTFIX d)
lcnc_enable_warnings(lasercnc_infrastructure)
