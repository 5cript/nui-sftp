# When this file is included its a flatpak build

# Header only:
add_library(boost_preprocessor INTERFACE IMPORTED GLOBAL)
add_library(mplex INTERFACE IMPORTED GLOBAL)
add_library(interval-tree INTERFACE IMPORTED GLOBAL)
add_library(boost_describe INTERFACE IMPORTED GLOBAL)
add_library(boost_mp11 INTERFACE IMPORTED GLOBAL)

if(EMSCRIPTEN)
    add_subdirectory("${CMAKE_CURRENT_BINARY_DIR}/flatpakdeps/nlohmann_json" EXCLUDE_FROM_ALL)
    add_subdirectory("${CMAKE_CURRENT_BINARY_DIR}/flatpakdeps/fmt" EXCLUDE_FROM_ALL)
else()
    find_package(nlohmann_json REQUIRED)
    find_package(fmt REQUIRED)
endif()