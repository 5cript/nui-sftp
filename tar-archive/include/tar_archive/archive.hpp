#pragma once

#include <tar_archive/compression.hpp>
#include <tar_archive/error.hpp>
#include <tar_archive/reader.hpp>
#include <tar_archive/writer.hpp>

#include <expected>
#include <filesystem>

namespace TarArchive
{
    /**
     * @brief Lazy archive-on-a-path reference.
     *
     * Construction performs no I/O. Call openReader() / openWriter() to actually touch
     * the filesystem; until then this is just a path holder with a preferred compression
     * override. This matches the "maybe-exists" semantics the caller asked for.
     */
    class Archive
    {
      public:
        /**
         * @brief Construct an archive reference for the given path. No I/O is performed.
         *
         * The path need not exist; for readers it must exist at openReader() time, for
         * writers it is created or truncated at openWriter() time.
         */
        explicit Archive(std::filesystem::path path) noexcept;

        /**
         * @brief Open the archive for reading.
         *
         * Verifies the file exists, detects compression (from extension, cross-checked
         * against the file's magic bytes), and returns a Reader positioned before the
         * first entry. Returns FileNotFound if the path does not exist, MagicMismatch if
         * the detected codec disagrees between extension and magic, or UnknownCompression
         * if neither source identifies a supported codec.
         *
         * When @p codecOverride is anything other than Compression::Auto, extension and
         * magic detection are both bypassed and the given codec is used verbatim.
         */
        std::expected<Reader, TarError> openReader(Compression codecOverride = Compression::Auto) const;

        /**
         * @brief Open the archive for writing (creates or truncates the file).
         *
         * Picks compression from the file extension unless @p codecOverride overrides it.
         * Returns UnknownCompression if Compression::Auto is requested but the extension
         * cannot be resolved. @p options tunes per-codec strength; unused fields are
         * silently ignored.
         */
        std::expected<Writer, TarError> openWriter(
            Compression codecOverride = Compression::Auto,
            CompressionOptions const& options = {}
        ) const;

        /** @brief The filesystem path this Archive refers to. */
        std::filesystem::path const& path() const noexcept { return path_; }

      private:
        std::filesystem::path path_;
    };
}
