# System SQLite via CMake's built-in FindSQLite3, not fetched.
if (NOT TARGET SQLite3::SQLite3)
    find_package(SQLite3 REQUIRED)
    # Older FindSQLite3 versions (e.g. CMake 4.1) only define SQLite::SQLite3.
    if (NOT TARGET SQLite3::SQLite3)
        add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
    endif()
endif()
