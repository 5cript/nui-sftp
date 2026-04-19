#include "../compression_internal.hpp"

#include <tar_archive/compression.hpp>
#include <tar_archive/error.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <string>
#include <string_view>

namespace TarArchive
{
    namespace
    {
        std::string toLower(std::string_view value)
        {
            std::string result;
            result.reserve(value.size());
            for (char character : value)
                result.push_back(static_cast<char>(
                    std::tolower(static_cast<unsigned char>(character))
                ));
            return result;
        }

        std::string lowercaseExtension(std::filesystem::path const& path)
        {
            return toLower(path.extension().string());
        }

        struct MagicSignature
        {
            Compression codec;
            std::array<std::uint8_t, 6u> bytes;
            std::size_t length;
        };

        constexpr std::array<MagicSignature, 4u> magicSignatures{{
            {Compression::Gzip,  {0x1Fu, 0x8Bu, 0u, 0u, 0u, 0u}, 2u},
            {Compression::Bzip2, {0x42u, 0x5Au, 0x68u, 0u, 0u, 0u}, 3u},
            {Compression::Zstd,  {0x28u, 0xB5u, 0x2Fu, 0xFDu, 0u, 0u}, 4u},
            {Compression::Xz,    {0xFDu, 0x37u, 0x7Au, 0x58u, 0x5Au, 0x00u}, 6u},
        }};

        Compression classifyMagic(std::span<std::uint8_t const> header) noexcept
        {
            for (auto const& signature : magicSignatures)
            {
                if (header.size() < signature.length)
                    continue;
                bool matches = true;
                for (std::size_t position = 0u; position < signature.length; ++position)
                {
                    if (header[position] != signature.bytes[position])
                    {
                        matches = false;
                        break;
                    }
                }
                if (matches)
                    return signature.codec;
            }
            return Compression::None;
        }
    }

    Compression compressionFromExtension(std::filesystem::path const& path) noexcept
    {
        const std::string extension = lowercaseExtension(path);

        if (extension == ".tar")
            return Compression::None;
        if (extension == ".gz" || extension == ".tgz")
            return Compression::Gzip;
        if (extension == ".bz2" || extension == ".tbz2" || extension == ".tbz")
            return Compression::Bzip2;
        if (extension == ".zst" || extension == ".tzst")
            return Compression::Zstd;
        if (extension == ".xz" || extension == ".txz")
            return Compression::Xz;

        return Compression::None;
    }

    std::expected<Compression, TarError>
    detectCompressionByMagic(std::filesystem::path const& path)
    {
        auto inputOrError = Detail::openInputFile(path);
        if (!inputOrError)
            return std::unexpected(inputOrError.error());

        std::array<std::uint8_t, 6u> header{};
        inputOrError->read(
            reinterpret_cast<char*>(header.data()),
            static_cast<std::streamsize>(header.size())
        );
        const auto bytesRead = static_cast<std::size_t>(inputOrError->gcount());
        if (inputOrError->bad())
            return std::unexpected(
                makeError(TarErrorCode::IoError, "could not read magic bytes", errno)
            );

        return classifyMagic(std::span<std::uint8_t const>{header.data(), bytesRead});
    }

    std::expected<std::unique_ptr<ByteSink>, TarError>
    makeSink(
        std::filesystem::path const& path, Compression codec, CompressionOptions const& options
    )
    {
        const Compression resolved =
            (codec == Compression::Auto) ? compressionFromExtension(path) : codec;

        auto outputOrError = Detail::openOutputFile(path);
        if (!outputOrError)
            return std::unexpected(outputOrError.error());

        switch (resolved)
        {
            case Compression::None:
                return Detail::makeRawSink(std::move(*outputOrError));
            case Compression::Gzip:
                return Detail::makeGzipSink(std::move(*outputOrError), options);
            case Compression::Bzip2:
                return Detail::makeBzip2Sink(std::move(*outputOrError), options);
            case Compression::Zstd:
                return Detail::makeZstdSink(std::move(*outputOrError), options);
            case Compression::Xz:
#ifdef TAR_ARCHIVE_WITH_XZ
                return Detail::makeXzSink(std::move(*outputOrError), options);
#else
                return std::unexpected(makeError(
                    TarErrorCode::UnknownCompression, "xz support not compiled in"
                ));
#endif
            case Compression::Auto:
                return std::unexpected(makeError(
                    TarErrorCode::UnknownCompression,
                    "compression could not be inferred from extension"
                ));
        }
        return std::unexpected(makeError(TarErrorCode::UnknownCompression, "unhandled codec"));
    }

    std::expected<std::unique_ptr<ByteSource>, TarError>
    makeSource(std::filesystem::path const& path, Compression codec)
    {
        Compression resolved = codec;
        if (resolved == Compression::Auto)
        {
            const Compression byExtension = compressionFromExtension(path);
            auto byMagic = detectCompressionByMagic(path);
            if (!byMagic)
                return std::unexpected(byMagic.error());
            if (byExtension != Compression::None && *byMagic != Compression::None &&
                byExtension != *byMagic)
                return std::unexpected(makeError(
                    TarErrorCode::MagicMismatch,
                    "file extension and magic bytes disagree on compression codec"
                ));
            resolved =
                (*byMagic != Compression::None) ? *byMagic : byExtension;
        }

        auto inputOrError = Detail::openInputFile(path);
        if (!inputOrError)
            return std::unexpected(inputOrError.error());

        switch (resolved)
        {
            case Compression::None:
                return Detail::makeRawSource(std::move(*inputOrError));
            case Compression::Gzip:
                return Detail::makeGzipSource(std::move(*inputOrError));
            case Compression::Bzip2:
                return Detail::makeBzip2Source(std::move(*inputOrError));
            case Compression::Zstd:
                return Detail::makeZstdSource(std::move(*inputOrError));
            case Compression::Xz:
#ifdef TAR_ARCHIVE_WITH_XZ
                return Detail::makeXzSource(std::move(*inputOrError));
#else
                return std::unexpected(makeError(
                    TarErrorCode::UnknownCompression, "xz support not compiled in"
                ));
#endif
            case Compression::Auto:
                return std::unexpected(makeError(
                    TarErrorCode::UnknownCompression,
                    "compression could not be inferred from extension + magic"
                ));
        }
        return std::unexpected(makeError(TarErrorCode::UnknownCompression, "unhandled codec"));
    }
}
