target_sources(
    lasercnc_kernel
    PRIVATE
        transaction/domain_event.cpp
        transaction/application_transaction.cpp
        transaction/transaction_manager.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
        FILES
            "${PROJECT_SOURCE_DIR}/include/lasercnc/messaging/domain_event.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/runtime/transaction.hpp"
            "${PROJECT_SOURCE_DIR}/include/lasercnc/runtime/transaction_manager.hpp"
)
