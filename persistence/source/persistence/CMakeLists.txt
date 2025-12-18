add_library(
    persistence
    STATIC
        state/ssh_options.cpp
        state/sftp_options.cpp
        state/ssh_session_options.cpp
        state/state.cpp
        state/terminal_engine.cpp
        state/terminal_options.cpp
        state/terminal_theme.cpp
        state/termios.cpp
        state/ui_options.cpp
        state/queue_options.cpp
        state/local_filesystem_options.cpp
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
endif()

target_include_directories(
    persistence
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_link_libraries(
    persistence
    PUBLIC
        constants
        utility
        log
        roar-include-only
        core-target
)