# System SQLite via CMake's built-in FindSQLite3, not fetched.
if (NOT TARGET SQLite3::SQLite3)
    find_package(SQLite3 REQUIRED)
endif()
