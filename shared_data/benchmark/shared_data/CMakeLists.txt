add_executable(
    nui-sftp-sync-benchmark
    sync_benchmark.cpp
)

target_link_libraries(nui-sftp-sync-benchmark PRIVATE shared-data)

set_target_properties(nui-sftp-sync-benchmark PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")
