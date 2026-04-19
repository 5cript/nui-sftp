include("${CMAKE_CURRENT_LIST_DIR}/../../../_cmake/dependencies/zlib.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../../../_cmake/dependencies/bzip2.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../../../_cmake/dependencies/zstd.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../../../_cmake/dependencies/liblzma.cmake")

set(TAR_ARCHIVE_SOURCES
    archive.cpp
    entry_reader.cpp
    entry_writer.cpp
    header.cpp
    reader.cpp
    writer.cpp
    compression/factory.cpp
    compression/raw_stream.cpp
    compression/gzip_stream.cpp
    compression/bzip2_stream.cpp
    compression/zstd_stream.cpp
)

if (TAR_ARCHIVE_HAS_XZ)
    list(APPEND TAR_ARCHIVE_SOURCES compression/xz_stream.cpp)
endif()

add_library(tar-archive STATIC ${TAR_ARCHIVE_SOURCES})

target_include_directories(
    tar-archive
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(
    tar-archive
    PRIVATE
        core-target
        ZLIB::ZLIB
        BZip2::BZip2
        zstd::libzstd
    PUBLIC
        shared-data
        utility
        fmt::fmt
)

if (TAR_ARCHIVE_HAS_XZ)
    target_link_libraries(tar-archive PRIVATE LibLZMA::LibLZMA)
    target_compile_definitions(tar-archive PRIVATE TAR_ARCHIVE_WITH_XZ=1)
endif()
