#pragma once

#include <tar_archive/error.hpp>
#include <utility/describe.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>

namespace TarArchive
{
    /**
     * @brief Identifies a compression codec.
     *
     * Auto means "pick from file extension (for writers) or from extension + magic bytes (for readers)".
     */
    enum class Compression
    {
        Auto,
        None,
        Gzip,
        Bzip2,
        Zstd,
        Xz
    };

    BOOST_DESCRIBE_ENUM(Compression, Auto, None, Gzip, Bzip2, Zstd, Xz)

    /**
     * @brief Per-codec tuning knobs for writers.
     *
     * Each field is optional: std::nullopt means "use the codec's own default". Only the
     * field matching the selected Compression is consulted; the rest are ignored. These
     * options are write-side only — readers do not take any tuning parameters.
     */
    struct CompressionOptions
    {
        /**
         * @brief gzip / zlib deflate level: 1 (fastest) … 9 (smallest). Default: 6.
         */
        std::optional<int> gzipLevel{};

        /**
         * @brief bzip2 block size in 100kB units: 1 … 9. Larger uses more memory and
         * compresses better. Default: 9.
         */
        std::optional<int> bzip2BlockSize{};

        /**
         * @brief bzip2 work factor: 0 … 250. 0 selects the library default (30). Affects
         * fallback behaviour for highly repetitive input.
         */
        std::optional<int> bzip2WorkFactor{};

        /**
         * @brief zstd compression level: 1 … 22 (levels above 19 enable long mode and use
         * much more memory). Default: ZSTD_CLEVEL_DEFAULT (3).
         */
        std::optional<int> zstdLevel{};

        /**
         * @brief xz preset level: 0 … 9. Add LZMA_PRESET_EXTREME by OR-ing into a raw
         * uint32 via xzExtreme. Default preset: 6.
         */
        std::optional<int> xzPreset{};

        /**
         * @brief If true, OR LZMA_PRESET_EXTREME into the xz preset. Slower but squeezes
         * a bit more off highly compressible input. Default: false.
         */
        bool xzExtreme{false};
    };

    /**
     * @brief Polymorphic write-only byte stream. The tar writer pushes raw tar bytes into this,
     * concrete implementations either pass them through or compress them.
     */
    class ByteSink
    {
      public:
        virtual ~ByteSink() = default;

        /**
         * @brief Consume bytes. May buffer internally; implementations do not guarantee flushing.
         */
        virtual std::expected<void, TarError> write(std::span<std::byte const> bytes) = 0;

        /**
         * @brief Flush all pending bytes, write any codec trailer, and close the underlying stream.
         * Must be called exactly once before destruction to produce a valid output; destructors
         * perform a best-effort finish but cannot surface errors.
         */
        virtual std::expected<void, TarError> finish() = 0;
    };

    /**
     * @brief Polymorphic read-only byte stream. The tar reader pulls bytes from this; concrete
     * implementations either pass through file bytes or decompress them.
     */
    class ByteSource
    {
      public:
        virtual ~ByteSource() = default;

        /**
         * @brief Read up to buf.size() bytes. Returns the number actually read.
         * A return value of 0 combined with eof() == true signals end of stream; 0 with
         * eof() == false means "try again" and should not normally happen for file-backed sources.
         */
        virtual std::expected<std::size_t, TarError> read(std::span<std::byte> buf) = 0;

        /**
         * @brief True once the underlying stream has no more bytes to yield.
         */
        virtual bool eof() const noexcept = 0;
    };

    /**
     * @brief Infer compression codec from the file extension. Returns Compression::None when
     * the extension is .tar (or unrecognised), Compression::Auto is never returned.
     */
    Compression compressionFromExtension(std::filesystem::path const& path) noexcept;

    /**
     * @brief Open a sink that writes to the given path, optionally compressed. Creates or
     * truncates the file. @p options is consulted only for the matching codec; all unused
     * fields are ignored.
     */
    std::expected<std::unique_ptr<ByteSink>, TarError>
    makeSink(std::filesystem::path const& path, Compression codec, CompressionOptions const& options = {});

    /**
     * @brief Open a source that reads from the given path, optionally decompressed.
     */
    std::expected<std::unique_ptr<ByteSource>, TarError>
    makeSource(std::filesystem::path const& path, Compression codec);

    /**
     * @brief Read the first bytes of the file and identify the compression codec from magic bytes.
     * Returns Compression::None if none of the supported magic sequences match.
     */
    std::expected<Compression, TarError> detectCompressionByMagic(std::filesystem::path const& path);
}
