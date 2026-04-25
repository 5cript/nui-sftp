target_sources(${PROJECT_NAME}
    PRIVATE
        main.cpp
        page_frame.cpp
        sections/nav_bar.cpp
        sections/hero.cpp
        sections/features.cpp
        sections/platforms.cpp
        sections/open_source.cpp
        sections/footer.cpp
)

set_common_options(${PROJECT_NAME})

target_link_libraries(${PROJECT_NAME} PRIVATE nui-frontend fmt script-nui-components)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../../include"
        "${CMAKE_BINARY_DIR}/include"
)

nui_prepare_emscripten_target(
    TARGET
        ${PROJECT_NAME}
    PREJS
        ${WEBPAGE_PREJS_FILE}
    UNPACKED_MODE
        on
    EMSCRIPTEN_LINK_OPTIONS
        -sEXPORTED_FUNCTIONS=_frontendMain
        -O2
        -sALLOW_MEMORY_GROWTH=1
    EMSCRIPTEN_COMPILE_OPTIONS
        -O3
    OBSERVED_BUNDLE_FILES
        "${CMAKE_SOURCE_DIR}/static/styles/styles.css"
        "${CMAKE_SOURCE_DIR}/../dependencies/5cript-nui-components/styles/carousel.css"
)
