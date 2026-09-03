add_library(lasercnc_foundation STATIC)
add_library(LaserCNC::Foundation ALIAS lasercnc_foundation)

target_sources(
    lasercnc_foundation
    PRIVATE
        value/value.cpp
        error/error.cpp
        schema/schema.cpp
        version/version.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
        FILES
            "${PROJECT_SOURCE_DIR}/include/lasercnc/foundation/error.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/foundation/result.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/foundation/schema.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/foundation/serialization.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/foundation/strong_id.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/foundation/value.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/foundation/version.hpp"
)

target_compile_features(lasercnc_foundation PUBLIC cxx_std_20)
set_target_properties(
    lasercnc_foundation
    PROPERTIES
        CXX_EXTENSIONS OFF
        DEBUG_POSTFIX d
)

lcnc_enable_warnings(lasercnc_foundation)
