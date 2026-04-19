#pragma once

#include <tar_archive/compression.hpp>

#include <filesystem>
#include <fstream>
#include <memory>

namespace TarArchive::Detail
{
    /** @brief Size of the scratch buffer used by every compressed sink/source. */
    inline constexpr std::size_t scratchBufferSize = 64u * 1024u;

    std::expected<std::ofstream, TarError> openOutputFile(std::filesystem::path const& path);
    std::expected<std::ifstream, TarError> openInputFile(std::filesystem::path const& path);

    std::unique_ptr<ByteSink> makeRawSink(std::ofstream output);
    std::unique_ptr<ByteSource> makeRawSource(std::ifstream input);

    std::expected<std::unique_ptr<ByteSink>, TarError>
    makeGzipSink(std::ofstream output, CompressionOptions const& options);
    std::expected<std::unique_ptr<ByteSource>, TarError> makeGzipSource(std::ifstream input);

    std::expected<std::unique_ptr<ByteSink>, TarError>
    makeBzip2Sink(std::ofstream output, CompressionOptions const& options);
    std::expected<std::unique_ptr<ByteSource>, TarError> makeBzip2Source(std::ifstream input);

    std::expected<std::unique_ptr<ByteSink>, TarError>
    makeZstdSink(std::ofstream output, CompressionOptions const& options);
    std::expected<std::unique_ptr<ByteSource>, TarError> makeZstdSource(std::ifstream input);

#ifdef TAR_ARCHIVE_WITH_XZ
    std::expected<std::unique_ptr<ByteSink>, TarError>
    makeXzSink(std::ofstream output, CompressionOptions const& options);
    std::expected<std::unique_ptr<ByteSource>, TarError> makeXzSource(std::ifstream input);
#endif
}
