# Compiles tsan_suppressions.txt into the ThreadSanitizer tests, so that a test binary run directly has them too.

set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/tsan_suppressions.txt")
file(READ "${CMAKE_CURRENT_LIST_DIR}/tsan_suppressions.txt" TSAN_SUPPRESSIONS)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/configured_files/tsan_default_suppressions.cpp.in"
    "${CMAKE_BINARY_DIR}/tsan_default_suppressions.cpp"
    @ONLY
)

function(add_tsan_default_suppressions target)
    target_sources(${target} PRIVATE "${CMAKE_BINARY_DIR}/tsan_default_suppressions.cpp")
endfunction()
