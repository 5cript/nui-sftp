include("${CMAKE_CURRENT_LIST_DIR}/../../../_cmake/dependencies/sqlite3.cmake")

add_library(
    command-store
    STATIC
        command_store.cpp
        command_store_rpc.cpp
        sqlite/error.cpp
        sqlite/statement.cpp
        sqlite/transaction.cpp
)

target_include_directories(
    command-store
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
        # rpc_helper.hpp for the rpc wrapper; header-only, so no backend link (would be circular).
        "${CMAKE_SOURCE_DIR}/backend/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(
    command-store
    PRIVATE
        core-target
        SQLite3::SQLite3
    PUBLIC
        utility
        log
        nui-backend
        nlohmann_json::nlohmann_json
        fmt::fmt
)
