#include "../compression_internal.hpp"

#include <tar_archive/compression.hpp>
#include <tar_archive/error.hpp>

#include <zstd.h>

#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

namespace TarArchive::Detail
{
    namespace
    {
        class ZstdSink final : public ByteSink
        {
          public:
            explicit ZstdSink(std::ofstream output) noexcept
                : output_{std::move(output)}
            {}

            std::expected<void, TarError> initialise(int compressionLevel)
            {
                context_ = ZSTD_createCStream();
                if (context_ == nullptr)
                    return std::unexpected(makeError(
                        TarErrorCode::CompressionLibraryError, "ZSTD_createCStream returned null"
                    ));
                const auto status = ZSTD_CCtx_setParameter(
                    context_, ZSTD_c_compressionLevel, compressionLevel
                );
                if (ZSTD_isError(status))
                    return std::unexpected(makeError(
                        TarErrorCode::CompressionLibraryError,
                        ZSTD_getErrorName(status),
                        static_cast<int>(status)
                    ));
                scratch_.resize(ZSTD_CStreamOutSize());
                return {};
            }

            ~ZstdSink() override
            {
                if (context_ != nullptr)
                {
                    if (!finished_)
                        (void)drainEnd();
                    ZSTD_freeCStream(context_);
                }
            }

            std::expected<void, TarError> write(std::span<std::byte const> bytes) override
            {
                if (finished_)
                    return std::unexpected(
                        makeError(TarErrorCode::AlreadyFinalized, "zstd sink already finished")
                    );

                ZSTD_inBuffer inBuffer{bytes.data(), bytes.size(), 0u};
                while (inBuffer.pos < inBuffer.size)
                {
                    ZSTD_outBuffer outBuffer{scratch_.data(), scratch_.size(), 0u};
                    const auto status = ZSTD_compressStream2(
                        context_, &outBuffer, &inBuffer, ZSTD_e_continue
                    );
                    if (ZSTD_isError(status))
                        return std::unexpected(makeError(
                            TarErrorCode::CompressionLibraryError,
                            ZSTD_getErrorName(status),
                            static_cast<int>(status)
                        ));
                    if (outBuffer.pos > 0u)
                    {
                        output_.write(
                            reinterpret_cast<char const*>(scratch_.data()),
                            static_cast<std::streamsize>(outBuffer.pos)
                        );
                        if (!output_)
                            return std::unexpected(makeError(
                                TarErrorCode::IoError, "zstd output write failed", errno
                            ));
                    }
                }
                return {};
            }

            std::expected<void, TarError> finish() override
            {
                if (finished_)
                    return std::unexpected(
                        makeError(TarErrorCode::AlreadyFinalized, "zstd sink already finished")
                    );
                const auto drained = drainEnd();
                if (!drained)
                    return drained;
                finished_ = true;
                output_.flush();
                if (!output_)
                    return std::unexpected(makeError(TarErrorCode::IoError, "flush failed", errno));
                output_.close();
                return {};
            }

          private:
            std::expected<void, TarError> drainEnd()
            {
                ZSTD_inBuffer inBuffer{nullptr, 0u, 0u};
                while (true)
                {
                    ZSTD_outBuffer outBuffer{scratch_.data(), scratch_.size(), 0u};
                    const auto remaining = ZSTD_compressStream2(
                        context_, &outBuffer, &inBuffer, ZSTD_e_end
                    );
                    if (ZSTD_isError(remaining))
                        return std::unexpected(makeError(
                            TarErrorCode::CompressionLibraryError,
                            ZSTD_getErrorName(remaining),
                            static_cast<int>(remaining)
                        ));
                    if (outBuffer.pos > 0u)
                    {
                        output_.write(
                            reinterpret_cast<char const*>(scratch_.data()),
                            static_cast<std::streamsize>(outBuffer.pos)
                        );
                        if (!output_)
                            return std::unexpected(makeError(
                                TarErrorCode::IoError, "zstd output write failed", errno
                            ));
                    }
                    if (remaining == 0u)
                        return {};
                }
            }

            std::ofstream output_;
            ZSTD_CStream* context_{nullptr};
            std::vector<std::byte> scratch_{};
            bool finished_{false};
        };

        class ZstdSource final : public ByteSource
        {
          public:
            explicit ZstdSource(std::ifstream input) noexcept
                : input_{std::move(input)}
            {}

            std::expected<void, TarError> initialise()
            {
                context_ = ZSTD_createDStream();
                if (context_ == nullptr)
                    return std::unexpected(makeError(
                        TarErrorCode::CompressionLibraryError, "ZSTD_createDStream returned null"
                    ));
                scratch_.resize(ZSTD_DStreamInSize());
                return {};
            }

            ~ZstdSource() override
            {
                if (context_ != nullptr)
                    ZSTD_freeDStream(context_);
            }

            std::expected<std::size_t, TarError> read(std::span<std::byte> target) override
            {
                if (target.empty() || eof_)
                    return std::size_t{0u};

                ZSTD_outBuffer outBuffer{target.data(), target.size(), 0u};
                while (outBuffer.pos < outBuffer.size && !eof_)
                {
                    if (inBuffer_.pos == inBuffer_.size)
                    {
                        if (inputEof_)
                        {
                            eof_ = true;
                            break;
                        }
                        input_.read(
                            reinterpret_cast<char*>(scratch_.data()),
                            static_cast<std::streamsize>(scratch_.size())
                        );
                        const auto fetched = static_cast<std::size_t>(input_.gcount());
                        if (input_.bad())
                            return std::unexpected(
                                makeError(TarErrorCode::IoError, "zstd input read failed", errno)
                            );
                        inBuffer_.src = scratch_.data();
                        inBuffer_.size = fetched;
                        inBuffer_.pos = 0u;
                        if (fetched == 0u)
                        {
                            inputEof_ = true;
                            eof_ = true;
                            break;
                        }
                    }

                    const auto status = ZSTD_decompressStream(context_, &outBuffer, &inBuffer_);
                    if (ZSTD_isError(status))
                        return std::unexpected(makeError(
                            TarErrorCode::CompressionLibraryError,
                            ZSTD_getErrorName(status),
                            static_cast<int>(status)
                        ));
                    if (status == 0u)
                    {
                        eof_ = true;
                        break;
                    }
                }
                return outBuffer.pos;
            }

            bool eof() const noexcept override { return eof_; }

          private:
            std::ifstream input_;
            ZSTD_DStream* context_{nullptr};
            std::vector<std::byte> scratch_{};
            ZSTD_inBuffer inBuffer_{nullptr, 0u, 0u};
            bool inputEof_{false};
            bool eof_{false};
        };
    }

    std::expected<std::unique_ptr<ByteSink>, TarError>
    makeZstdSink(std::ofstream output, CompressionOptions const& options)
    {
        const int compressionLevel = options.zstdLevel.value_or(ZSTD_CLEVEL_DEFAULT);
        auto sink = std::make_unique<ZstdSink>(std::move(output));
        const auto initialised = sink->initialise(compressionLevel);
        if (!initialised)
            return std::unexpected(initialised.error());
        return sink;
    }

    std::expected<std::unique_ptr<ByteSource>, TarError> makeZstdSource(std::ifstream input)
    {
        auto source = std::make_unique<ZstdSource>(std::move(input));
        const auto initialised = source->initialise();
        if (!initialised)
            return std::unexpected(initialised.error());
        return source;
    }
}
