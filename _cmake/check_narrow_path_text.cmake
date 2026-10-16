# Fails when native first-party sources call string() or generic_string(), which convert a path through the ANSI code
# page on Windows and throw outside it. Paths::nativeToUtf8 and Paths::nativeFromUtf8 convert explicitly. Frontend
# sources are not checked, a path there is UTF-8 already. Run with -DSOURCE_DIR=<repository root>.

set(roots
    backend
    command-store/include
    command-store/source
    log
    nui-file-explorer/source/nui-file-explorer/support
    paths/include
    persistence/include
    persistence/source
    shared_data/include
    shared_data/source
    ssh/include
    ssh/source
    tar-archive/include
    tar-archive/source
    utility/include
    utility/source
)

set(found "")
foreach(root IN LISTS roots)
    file(GLOB_RECURSE sources "${SOURCE_DIR}/${root}/*.cpp" "${SOURCE_DIR}/${root}/*.hpp" "${SOURCE_DIR}/${root}/*.mm")
    foreach(source IN LISTS sources)
        file(RELATIVE_PATH relative "${SOURCE_DIR}" "${source}")
        if (relative MATCHES "(^|/)test/")
            continue()
        endif()
        file(STRINGS "${source}" lines REGEX "\\.(generic_)?string\\(\\)")
        foreach(line IN LISTS lines)
            if (NOT line MATCHES "^[ \t]*//")
                string(STRIP "${line}" stripped)
                string(APPEND found "\n  ${relative}: ${stripped}")
            endif()
        endforeach()
    endforeach()
endforeach()

if (found)
    message(FATAL_ERROR "Narrow path conversions, use Paths::nativeToUtf8 or Paths::nativeFromUtf8:${found}")
endif()
message(STATUS "No narrow path conversions in native sources.")
