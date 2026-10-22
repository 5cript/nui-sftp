if (FETCH_EFSW)
    include(FetchContent)
    # If you edit here, update work_dependencies.json
    FetchContent_Declare(
        efsw
        GIT_REPOSITORY https://github.com/5cript/efsw.git
        GIT_TAG        627eb0d2087360d59abbbb3ea7c9b951407bc946
    )

    FetchContent_MakeAvailable(efsw)
endif()