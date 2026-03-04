if (FETCH_YAML_CPP)
  include(FetchContent)

  FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG 89ff142b991af432b5d7a7cee55282f082a7e629
  )
  FetchContent_MakeAvailable(yaml-cpp)
endif()