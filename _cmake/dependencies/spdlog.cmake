set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "Use an external fmt library (provide it manually)" FORCE)

if (FETCH_SPDLOG)
    include(FetchContent)
    # If you edit here, update work_dependencies.json
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        d5af52d903ed69f96c7350155e94aeff248b31eb
    )

    FetchContent_MakeAvailable(spdlog)
endif()