#include "../compression_internal.hpp"

#include <tar_archive/compression.hpp>
#include <tar_archive/error.hpp>

#include <bzlib.h>

#include <array>
#include <cstring>
#include <fstream>
#include <utility>

namespace TarArchive::Detail
{
    namespace
    {
        class Bzip2Sink final : public ByteSink
        {
          public:
            explicit Bzip2Sink(std::ofstream output) noexcept
                : output_{std::move(output)}
            {}

            std::expected<void, TarError> initialise(int blockSize, int workFactor)
            {
                const int status = BZ2_bzCompressInit(&stream_, blockSize, 0, workFactor);
                if (status != BZ_OK)
                    return std::unexpected(makeError(
                        TarErrorCode::CompressionLibraryError, "BZ2_bzCompressInit failed", status
                    ));
                initialised_ = true;
                return {};
            }

            ~Bzip2Sink() override
            {
                if (initialised_ && !finished_)
                    (void)pump(BZ_FINISH);
                if (initialised_)
                    BZ2_bzCompressEnd(&stream_);
            }

            std::expected<void, TarError> write(std::span<std::byte const> bytes) override
            {
                if (finished_)
                    return std::unexpected(
                        makeError(TarErrorCode::AlreadyFinalized, "bzip2 sink already finished")
                    );
                stream_.next_in = const_cast<char*>(reinterpret_cast<char const*>(bytes.data()));
                stream_.avail_in = static_cast<unsigned int>(bytes.size());
                return pump(BZ_RUN);
            }

            std::expected<void, TarError> finish() override
            {
                if (finished_)
                    return std::unexpected(
                        makeError(TarErrorCode::AlreadyFinalized, "bzip2 sink already finished")
                    );
                stream_.next_in = nullptr;
                stream_.avail_in = 0u;
                const auto flushed = pump(BZ_FINISH);
                if (!flushed)
                    return flushed;
                finished_ = true;
                output_.flush();
                if (!output_)
                    return std::unexpected(makeError(TarErrorCode::IoError, "flush failed", errno));
                output_.close();
                return {};
            }

          private:
            std::expected<void, TarError> pump(int action)
            {
                while (true)
                {
                    stream_.next_out = reinterpret_cast<char*>(scratch_.data());
                    stream_.avail_out = static_cast<unsigned int>(scratch_.size());

                    const int status = BZ2_bzCompress(&stream_, action);
                    if (status < 0)
                        return std::unexpected(makeError(
                            TarErrorCode::CompressionLibraryError, "BZ2_bzCompress failed", status
                        ));

                    const std::size_t produced = scratch_.size() - stream_.avail_out;
                    if (produced > 0u)
                    {
                        output_.write(
                            reinterpret_cast<char const*>(scratch_.data()),
                            static_cast<std::streamsize>(produced)
                        );
                        if (!output_)
                            return std::unexpected(makeError(
                                TarErrorCode::IoError, "bzip2 output write failed", errno
                            ));
                    }

                    if (action == BZ_RUN && stream_.avail_in == 0u)
                        return {};
                    if (action == BZ_FINISH && status == BZ_STREAM_END)
                        return {};
                    if (produced == 0u && stream_.avail_in == 0u)
                        return {};
                }
            }

            std::ofstream output_;
            bz_stream stream_{};
            std::array<std::byte, scratchBufferSize> scratch_{};
            bool initialised_{false};
            bool finished_{false};
        };

        class Bzip2Source final : public ByteSource
        {
          public:
            explicit Bzip2Source(std::ifstream input) noexcept
                : input_{std::move(input)}
            {}

            std::expected<void, TarError> initialise()
            {
                const int status = BZ2_bzDecompressInit(&stream_, 0, 0);
                if (status != BZ_OK)
                    return std::unexpected(makeError(
                        TarErrorCode::CompressionLibraryError, "BZ2_bzDecompressInit failed", status
                    ));
                initialised_ = true;
                return {};
            }

            ~Bzip2Source() override
            {
                if (initialised_)
                    BZ2_bzDecompressEnd(&stream_);
            }

            std::expected<std::size_t, TarError> read(std::span<std::byte> target) override
            {
                if (target.empty() || eof_)
                    return std::size_t{0u};

                stream_.next_out = reinterpret_cast<char*>(target.data());
                stream_.avail_out = static_cast<unsigned int>(target.size());

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
                                makeError(TarErrorCode::IoError, "bzip2 input read failed", errno)
                            );
                        if (fetched == 0u)
                            inputEof_ = true;
                        stream_.next_in = reinterpret_cast<char*>(scratch_.data());
                        stream_.avail_in = static_cast<unsigned int>(fetched);
                    }

                    const int status = BZ2_bzDecompress(&stream_);
                    if (status == BZ_STREAM_END)
                    {
                        eof_ = true;
                        break;
                    }
                    if (status != BZ_OK)
                        return std::unexpected(makeError(
                            TarErrorCode::CompressionLibraryError, "BZ2_bzDecompress failed", status
                        ));
                    if (stream_.avail_in == 0u && inputEof_ && target.size() - stream_.avail_out == 0u)
                    {
                        eof_ = true;
                        break;
                    }
                }
                return target.size() - stream_.avail_out;
            }

            bool eof() const noexcept override { return eof_; }

          private:
            std::ifstream input_;
            bz_stream stream_{};
            std::array<std::byte, scratchBufferSize> scratch_{};
            bool initialised_{false};
            bool inputEof_{false};
            bool eof_{false};
        };
    }

    std::expected<std::unique_ptr<ByteSink>, TarError>
    makeBzip2Sink(std::ofstream output, CompressionOptions const& options)
    {
        const int blockSize = options.bzip2BlockSize.value_or(9);
        const int workFactor = options.bzip2WorkFactor.value_or(0);
        auto sink = std::make_unique<Bzip2Sink>(std::move(output));
        const auto initialised = sink->initialise(blockSize, workFactor);
        if (!initialised)
            return std::unexpected(initialised.error());
        return sink;
    }

    std::expected<std::unique_ptr<ByteSource>, TarError> makeBzip2Source(std::ifstream input)
    {
        auto source = std::make_unique<Bzip2Source>(std::move(input));
        const auto initialised = source->initialise();
        if (!initialised)
            return std::unexpected(initialised.error());
        return source;
    }
}
