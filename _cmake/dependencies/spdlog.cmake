set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "Use an external fmt library (provide it manually)" FORCE)

if (FETCH_SPDLOG)
    include(FetchContent)
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        v1.17.0
    )

    FetchContent_MakeAvailable(spdlog)
endif()