add_library(
    log
    STATIC
        log.cpp
)
target_include_directories(log PUBLIC "${CMAKE_SOURCE_DIR}/log/include")

if (EMSCRIPTEN)
    target_link_libraries(
        log
        PUBLIC
            core-target
            utility
            fmt
            nui-frontend
    )
    if (WIN32)
        target_compile_options(fmt PRIVATE -sMEMORY64=1)
        target_link_options(fmt PRIVATE -sMEMORY64=1)
    else()
        # No support for WASM64 in webkit
        target_link_options(
            fmt
            PRIVATE
                -sWASM_BIGINT=1
        )
    endif()
else()
    target_link_libraries(
        log
        PUBLIC
            core-target
            utility
            spdlog::spdlog
            nui-backend
        PRIVATE
            roar::no-net
            constants
    )
    target_sources(
        log
        PRIVATE
            logger_backend.cpp
    )
endif()