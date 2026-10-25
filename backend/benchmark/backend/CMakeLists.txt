add_executable(
    nui-sftp-transfer-benchmark
    main.cpp
    application_client.cpp
    connection.cpp
    libssh_client.cpp
    openssh_client.cpp
    openssh_server.cpp
    operation_driver.cpp
    report.cpp
    workload.cpp
)

target_link_libraries(
    nui-sftp-transfer-benchmark
    PRIVATE
        backend
        secure-shell
        docker-control
        utility
)

target_compile_definitions(
    nui-sftp-transfer-benchmark
    PRIVATE NUI_SFTP_BENCHMARK_DOCKER_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/docker"
)

set_target_properties(nui-sftp-transfer-benchmark PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmark")

# Only on demand: cmake --build <dir> --target run-transfer-benchmark. Not registered with ctest.
add_custom_target(
    run-transfer-benchmark
    COMMAND nui-sftp-transfer-benchmark
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/benchmark"
    USES_TERMINAL
    VERBATIM
)
