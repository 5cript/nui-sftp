#include <tar_archive/archive.hpp>
#include <tar_archive/compression.hpp>

#include "compression_internal.hpp"

#include <utility>

namespace TarArchive
{
    Archive::Archive(std::filesystem::path path) noexcept
        : path_{std::move(path)}
    {}

    std::expected<Reader, TarError> Archive::openReader(Compression codecOverride) const
    {
        Compression resolved = codecOverride;
        if (resolved == Compression::Auto)
        {
            if (!std::filesystem::exists(path_))
                return std::unexpected(makeError(
                    TarErrorCode::FileNotFound, "archive does not exist: " + path_.string()
                ));
            const Compression byExtension = compressionFromExtension(path_);
            auto byMagic = detectCompressionByMagic(path_);
            if (!byMagic)
                return std::unexpected(byMagic.error());
            if (byExtension != Compression::None && *byMagic != Compression::None &&
                byExtension != *byMagic)
                return std::unexpected(makeError(
                    TarErrorCode::MagicMismatch,
                    "file extension and magic bytes disagree on compression codec"
                ));
            resolved = (*byMagic != Compression::None) ? *byMagic : byExtension;
        }

        auto source = makeSource(path_, resolved);
        if (!source)
            return std::unexpected(source.error());
        return Reader::makeFromSource(std::move(*source));
    }

    std::expected<Writer, TarError>
    Archive::openWriter(Compression codecOverride, CompressionOptions const& options) const
    {
        Compression resolved = codecOverride;
        if (resolved == Compression::Auto)
            resolved = compressionFromExtension(path_);

        auto sink = makeSink(path_, resolved, options);
        if (!sink)
            return std::unexpected(sink.error());
        return Writer::makeFromSink(std::move(*sink));
    }
}
