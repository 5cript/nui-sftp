add_library(
    docker-control
    STATIC
        docker.cpp
        container.cpp
)

target_include_directories(docker-control PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../../include")

find_package(fmt CONFIG REQUIRED)
target_link_libraries(docker-control PUBLIC core-target fmt::fmt)
