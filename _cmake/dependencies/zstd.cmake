# zstd ships a CMake config (zstdConfig.cmake) in recent versions.
# Fall back to pkg-config for older installs.
find_package(zstd CONFIG QUIET)

if (NOT zstd_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(zstd REQUIRED IMPORTED_TARGET libzstd)

    if (NOT TARGET zstd::libzstd)
        add_library(zstd::libzstd INTERFACE IMPORTED)
        target_link_libraries(zstd::libzstd INTERFACE PkgConfig::zstd)
    endif()
endif()

# Normalise target name: downstream always links against zstd::libzstd.
if (NOT TARGET zstd::libzstd)
    if (TARGET zstd::libzstd_shared)
        add_library(zstd::libzstd ALIAS zstd::libzstd_shared)
    elseif (TARGET zstd::libzstd_static)
        add_library(zstd::libzstd ALIAS zstd::libzstd_static)
    endif()
endif()
