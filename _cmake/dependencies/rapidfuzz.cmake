if (FETCH_RAPIDFUZZ)
    include(FetchContent)
    # If you edit here, update work_dependencies.json
    FetchContent_Declare(
        rapidfuzz
        GIT_REPOSITORY https://github.com/rapidfuzz/rapidfuzz-cpp.git
        GIT_TAG b8ce411e91e01599d0697ad307933e05ddf3a723
    )
    FetchContent_MakeAvailable(rapidfuzz)
endif()