if (FETCH_EFSW)
    include(FetchContent)
    # If you edit here, update work_dependencies.json
    FetchContent_Declare(
        efsw
        GIT_REPOSITORY https://github.com/5cript/efsw.git
        GIT_TAG        d71ed846811cc67ee8827614341e2777d200231a
    )

    FetchContent_MakeAvailable(efsw)
endif()