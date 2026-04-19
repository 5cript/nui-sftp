#include "../compression_internal.hpp"

#include <tar_archive/compression.hpp>
#include <tar_archive/error.hpp>

#include <zlib.h>

#include <array>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <utility>

namespace TarArchive::Detail
{
    namespace
    {
        /** @brief windowBits value that selects gzip framing rather than raw zlib. */
        constexpr int gzipWindowBits = 15 + 16;

        class GzipSink final : public ByteSink
        {
          public:
            explicit GzipSink(std::unique_ptr<ByteSink> downstream)
                : output_{std::move(downstream)}
            {
                stream_.zalloc = Z_NULL;
                stream_.zfree = Z_NULL;
                stream_.opaque = Z_NULL;
            }

            std::expected<void, TarError> initialise(int compressionLevel)
            {
                const int status = deflateInit2(
                    &stream_, compressionLevel, Z_DEFLATED, gzipWindowBits, 8, Z_DEFAULT_STRATEGY
                );
                if (status != Z_OK)
                    return std::unexpected(makeError(
                        TarErrorCode::CompressionLibraryError, "deflateInit2 failed", status
                    ));
                initialised_ = true;
                return {};
            }

            ~GzipSink() override
            {
                if (initialised_ && !finished_)
                    (void)pump(Z_FINISH);
                if (initialised_)
                    deflateEnd(&stream_);
            }

            std::expected<void, TarError> write(std::span<std::byte const> bytes) override
            {
                if (finished_)
                    return std::unexpected(
                        makeError(TarErrorCode::AlreadyFinalized, "gzip sink already finished")
                    );
                stream_.next_in = const_cast<Bytef*>(reinterpret_cast<Bytef const*>(bytes.data()));
                stream_.avail_in = static_cast<uInt>(bytes.size());
                return pump(Z_NO_FLUSH);
            }

            std::expected<void, TarError> finish() override
            {
                if (finished_)
                    return std::unexpected(
                        makeError(TarErrorCode::AlreadyFinalized, "gzip sink already finished")
                    );
                stream_.next_in = nullptr;
                stream_.avail_in = 0u;
                const auto flushed = pump(Z_FINISH);
                if (!flushed)
                    return flushed;
                finished_ = true;
                return output_->finish();
            }

          private:
            std::expected<void, TarError> pump(int flushMode)
            {
                while (true)
                {
                    stream_.next_out = reinterpret_cast<Bytef*>(scratch_.data());
                    stream_.avail_out = static_cast<uInt>(scratch_.size());

                    const int status = deflate(&stream_, flushMode);
                    if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR)
                        return std::unexpected(makeError(
                            TarErrorCode::CompressionLibraryError, "deflate failed", status
                        ));

                    const std::size_t produced = scratch_.size() - stream_.avail_out;
                    if (produced > 0u)
                    {
                        auto downstreamResult = output_->write(
                            std::span<std::byte const>{scratch_.data(), produced}
                        );
                        if (!downstreamResult)
                            return std::unexpected(downstreamResult.error());
                    }

                    if (status == Z_STREAM_END)
                        return {};
                    if (flushMode == Z_NO_FLUSH && stream_.avail_in == 0u && produced == 0u)
                        return {};
                    if (flushMode == Z_FINISH && status == Z_BUF_ERROR && produced == 0u)
                        return {};
                }
            }

            std::unique_ptr<ByteSink> output_;
            z_stream stream_{};
            std::array<std::byte, scratchBufferSize> scratch_{};
            bool initialised_{false};
            bool finished_{false};
        };

        class GzipSource final : public ByteSource
        {
          public:
            explicit GzipSource(std::ifstream input)
                : input_{std::move(input)}
            {
                stream_.zalloc = Z_NULL;
                stream_.zfree = Z_NULL;
                stream_.opaque = Z_NULL;
            }

            std::expected<void, TarError> initialise()
            {
                const int status = inflateInit2(&stream_, gzipWindowBits);
                if (status != Z_OK)
                    return std::unexpected(makeError(
                        TarErrorCode::CompressionLibraryError, "inflateInit2 failed", status
                    ));
                initialised_ = true;
                return {};
            }

            ~GzipSource() override
            {
                if (initialised_)
                    inflateEnd(&stream_);
            }

            std::expected<std::size_t, TarError> read(std::span<std::byte> target) override
            {
                if (target.empty() || eof_)
                    return std::size_t{0u};

                stream_.next_out = reinterpret_cast<Bytef*>(target.data());
                stream_.avail_out = static_cast<uInt>(target.size());

                while (stream_.avail_out > 0u)
                {
                    if (stream_.avail_in == 0u && !inputEof_)
                    {
                        input_.read(
                            reinterpret_cast<char*>(scratch_.data()),
                            static_cast<std::streamsize>(scratch_.size())
                        );
                        const auto fetched = static_cast<std::size_t>(input_.gcount());
                        if (input_.bad())
                            return std::unexpected(
                                makeError(TarErrorCode::IoError, "gzip input read failed", errno)
                            );
                        if (fetched == 0u)
                            inputEof_ = true;
                        stream_.next_in = reinterpret_cast<Bytef*>(scratch_.data());
                        stream_.avail_in = static_cast<uInt>(fetched);
                    }

                    const int status = inflate(&stream_, Z_NO_FLUSH);
                    if (status == Z_STREAM_END)
                    {
                        eof_ = true;
                        break;
                    }
                    if (status == Z_BUF_ERROR)
                    {
                        if (inputEof_)
                        {
                            eof_ = true;
                            break;
                        }
                        continue;
                    }
                    if (status != Z_OK)
                        return std::unexpected(makeError(
                            TarErrorCode::CompressionLibraryError, "inflate failed", status
                        ));
                }
                return target.size() - stream_.avail_out;
            }

            bool eof() const noexcept override { return eof_; }

          private:
            std::ifstream input_;
            z_stream stream_{};
            std::array<std::byte, scratchBufferSize> scratch_{};
            bool initialised_{false};
            bool inputEof_{false};
            bool eof_{false};
        };
    }

    std::expected<std::unique_ptr<ByteSink>, TarError>
    makeGzipSink(std::unique_ptr<ByteSink> downstream, CompressionOptions const& options)
    {
        const int compressionLevel = options.gzipLevel.value_or(Z_DEFAULT_COMPRESSION);
        auto sink = std::make_unique<GzipSink>(std::move(downstream));
        const auto initialised = sink->initialise(compressionLevel);
        if (!initialised)
            return std::unexpected(initialised.error());
        return sink;
    }

    std::expected<std::unique_ptr<ByteSource>, TarError> makeGzipSource(std::ifstream input)
    {
        auto source = std::make_unique<GzipSource>(std::move(input));
        const auto initialised = source->initialise();
        if (!initialised)
            return std::unexpected(initialised.error());
        return source;
    }
}
