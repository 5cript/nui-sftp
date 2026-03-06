if (FETCH_EFSW)
    include(FetchContent)
    # If you edit here, update work_dependencies.json
    FetchContent_Declare(
        efsw
        GIT_REPOSITORY https://github.com/SpartanJ/efsw.git
        GIT_TAG        87abe599995d5646f5d83cf2e3a225bd73148b3a
    )

    FetchContent_MakeAvailable(efsw)
endif()