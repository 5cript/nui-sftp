add_library(events STATIC app_event_context.cpp)

target_include_directories(events
    PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../../include")

target_link_libraries(events PUBLIC nui-events PRIVATE core-target)

if (EMSCRIPTEN)
    if (WIN32)
        target_compile_options(nui-events PRIVATE -sMEMORY64=1)
        target_link_options(nui-events PRIVATE -sMEMORY64=1)
    else()
        # No support for WASM64 in webkit
        target_link_options(
            nui-events
            PRIVATE
                -sWASM_BIGINT=1
        )
    endif()
endif()