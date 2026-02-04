option(NUI_FILE_EXPLORER_MEMORY64 "Enable MEMORY64 for nui-file-explorer" OFF)

add_library(
    nui-file-explorer
    STATIC
        "${CMAKE_CURRENT_LIST_DIR}/file_grid.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/dropdown_menu.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/side.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/flavor.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/drop_event_extractor.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/path_suggestion_cache.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/side/flavor_implementation.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/side/icon_flavor.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/side/table_flavor.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/side/selection_manager.cpp"
)

set_source_files_properties(
    "${CMAKE_CURRENT_LIST_DIR}/file_grid.cpp"
    PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../../styles/file_grid.css"
)

target_link_libraries(
    nui-file-explorer
    PRIVATE
        nui-frontend
        utility
        shared-data
        rapidfuzz::rapidfuzz
)

target_include_directories(
    nui-file-explorer
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/../../include
)

if (NUI_FILE_EXPLORER_MEMORY64)
    if (WIN32)
        target_compile_options(nui-file-explorer PRIVATE 
            -sMEMORY64=1
        )
        target_link_options(nui-file-explorer PRIVATE 
            -sMEMORY64=1
        )
    else()
        # No support for WASM64 in webkit
        target_link_options(nui-file-explorer PRIVATE 
            -sWASM_BIGINT=1
        )
    endif()
endif()

target_compile_definitions(nui-file-explorer PRIVATE -DBROWSER_ENGINE=${BROWSER_ENGINE})