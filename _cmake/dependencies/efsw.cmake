if (FETCH_EFSW)
    include(FetchContent)
    # If you edit here, update work_dependencies.json
    FetchContent_Declare(
        efsw
        GIT_REPOSITORY https://github.com/5cript/efsw.git
        GIT_TAG        e733a678fac0437bdd170cdb21ad5cb7ecd8ea81
    )

    FetchContent_MakeAvailable(efsw)
endif()