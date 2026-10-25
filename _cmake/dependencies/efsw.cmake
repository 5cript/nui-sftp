if (FETCH_EFSW)
    include(FetchContent)
    # If you edit here, update work_dependencies.json
    FetchContent_Declare(
        efsw
        GIT_REPOSITORY https://github.com/5cript/efsw.git
        GIT_TAG        4b18f1a91a54db1238b31a7b1bdf8380c335fd45
    )

    FetchContent_MakeAvailable(efsw)
endif()