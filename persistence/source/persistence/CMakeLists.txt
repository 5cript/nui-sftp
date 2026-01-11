add_library(
    persistence
    STATIC
        state/state.cpp
        state/terminal_engine.cpp
        state/termios.cpp
        state_holder.cpp
)

if (EMSCRIPTEN)
    target_sources(
        persistence
        PRIVATE
            state_holder_frontend.cpp
    )
else()
    target_sources(
        persistence
        PRIVATE
            state_holder_backend.cpp
    )
    target_link_libraries(persistence PRIVATE yaml-cpp::yaml-cpp)
endif()

target_include_directories(
    persistence
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_link_libraries(
    persistence
    PRIVATE
        core-target
    PUBLIC
        constants
        utility
        shared-data
        log
        roar-include-only
)