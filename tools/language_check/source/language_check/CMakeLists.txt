cmake_minimum_required(VERSION 4.1)

project(language-check LANGUAGES CXX)

add_executable(language-check
    main.cpp
    "source_files.cpp"
)

find_package(Boost CONFIG 1.86.0 REQUIRED COMPONENTS filesystem process)

target_link_libraries(language-check PRIVATE core-target)

target_include_directories(
    language-check
    PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../../include"
        "${CMAKE_BINARY_DIR}/include"
)

target_link_libraries(
    language-check
    PUBLIC
        Boost::filesystem
        Boost::process
        yaml-cpp
        nlohmann_json::nlohmann_json
)

set_target_properties(language-check
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools_bin"
)