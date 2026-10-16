add_library(paths INTERFACE)

target_include_directories(paths INTERFACE "${CMAKE_CURRENT_LIST_DIR}/../../include")
target_link_libraries(paths INTERFACE nlohmann_json::nlohmann_json)
