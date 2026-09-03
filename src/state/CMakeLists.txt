target_sources(
    lasercnc_kernel
    PRIVATE
        document/document.cpp
        document/document_store.cpp
        object_registry/object_registry.cpp
        object_type_registry/object_type_registry.cpp
        revision/revision.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
        FILES
            "${PROJECT_SOURCE_DIR}/include/lasercnc/state/document.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/state/asset_ref.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/state/document_store.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/state/object_registry.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/state/object_type_registry.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/state/revision.hpp"
)
