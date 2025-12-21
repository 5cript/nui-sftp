add_library(
    utility
    STATIC
        temporary_directory.cpp
)

target_include_directories(utility PUBLIC ${CMAKE_SOURCE_DIR}/utility/include)

include("${CMAKE_CURRENT_LIST_DIR}/../../../dependencies/Nui/cmake/dependencies/nlohmann_json.cmake")

if(EMSCRIPTEN)
    # This allows for better exception handling in frontend parts.
    target_link_libraries(utility PUBLIC nui-frontend)
else()
    target_sources(
        utility
        PRIVATE
        node/node.cpp
    )

    find_package(Boost CONFIG 1.89.0 REQUIRED COMPONENTS process)

    find_program(NODE node)
    find_program(NPM npm)
    find_program(CMD cmd)

    target_link_libraries(utility PUBLIC nui-backend Boost::process)
endif()

target_compile_definitions(utility PRIVATE -DNODE_EXECUTABLE="${NODE}" -DNPM_EXECUTABLE="${NPM}")
target_compile_definitions(utility PRIVATE -DCMD_EXECUTABLE="${CMD}" -DMSYS2_BASH="${MSYS2_BASH}")

target_link_libraries(utility PUBLIC
    core-target
    nlohmann_json::nlohmann_json
)