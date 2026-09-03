include(FetchContent)

# Development dependency only.
# Catch2 v3.15.3, pinned to the immutable upstream commit.
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG 95d8a61b089317bec800c7cc4c64064cbcb3802d
    GIT_SHALLOW FALSE
    EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
