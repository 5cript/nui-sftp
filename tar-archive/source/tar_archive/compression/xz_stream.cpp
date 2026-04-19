#ifdef TAR_ARCHIVE_WITH_XZ

#include "../compression_internal.hpp"

#include <tar_archive/compression.hpp>
#include <tar_archive/error.hpp>

#include <lzma.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <utility>

namespace TarArchive::Detail
{
    namespace
    {
        class XzSink final : public ByteSink
        {
          public:
            explicit XzSink(std::unique_ptr<ByteSink> downstream) noexcept
                : output_{std::move(downstream)}
            {}

            std::expected<void, TarError> initialise(std::uint32_t preset)
            {
                const lzma_ret status = lzma_easy_encoder(&stream_, preset, LZMA_CHECK_CRC64);
                if (status != LZMA_OK)
                    return std::unexpected(makeError(
                        TarErrorCode::CompressionLibraryError,
                        "lzma_easy_encoder failed",
                        static_cast<int>(status)
                    ));
                initialised_ = true;
                return {};
            }

            ~XzSink() override
            {
                if (initialised_ && !finished_)
                    (void)pump(LZMA_FINISH);
                if (initialised_)
                    lzma_end(&stream_);
            }

            std::expected<void, TarError> write(std::span<std::byte const> bytes) override
            {
                if (finished_)
                    return std::unexpected(
                        makeError(TarErrorCode::AlreadyFinalized, "xz sink already finished")
                    );
                stream_.next_in = reinterpret_cast<std::uint8_t const*>(bytes.data());
                stream_.avail_in = bytes.size();
                return pump(LZMA_RUN);
            }

            std::expected<void, TarError> finish() override
            {
                if (finished_)
                    return std::unexpected(
                        makeError(TarErrorCode::AlreadyFinalized, "xz sink already finished")
                    );
                stream_.next_in = nullptr;
                stream_.avail_in = 0u;
                const auto flushed = pump(LZMA_FINISH);
                if (!flushed)
                    return flushed;
                finished_ = true;
                return output_->finish();
            }

          private:
            std::expected<void, TarError> pump(lzma_action action)
            {
                while (true)
                {
                    stream_.next_out = reinterpret_cast<std::uint8_t*>(scratch_.data());
                    stream_.avail_out = scratch_.size();

                    const lzma_ret status = lzma_code(&stream_, action);
                    if (status != LZMA_OK && status != LZMA_STREAM_END)
                        return std::unexpected(makeError(
                            TarErrorCode::CompressionLibraryError,
                            "lzma_code failed",
                            static_cast<int>(status)
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

                    if (status == LZMA_STREAM_END)
                        return {};
                    if (action == LZMA_RUN && stream_.avail_in == 0u)
                        return {};
                    if (produced == 0u && stream_.avail_in == 0u)
                        return {};
                }
            }

            std::unique_ptr<ByteSink> output_;
            lzma_stream stream_ = LZMA_STREAM_INIT;
            std::array<std::byte, scratchBufferSize> scratch_{};
            bool initialised_{false};
            bool finished_{false};
        };

        class XzSource final : public ByteSource
        {
          public:
            explicit XzSource(std::ifstream input) noexcept
                : input_{std::move(input)}
            {}

            std::expected<void, TarError> initialise()
            {
                const lzma_ret status = lzma_stream_decoder(&stream_, UINT64_MAX, 0u);
                if (status != LZMA_OK)
                    return std::unexpected(makeError(
                        TarErrorCode::CompressionLibraryError,
                        "lzma_stream_decoder failed",
                        static_cast<int>(status)
                    ));
                initialised_ = true;
                return {};
            }

            ~XzSource() override
            {
                if (initialised_)
                    lzma_end(&stream_);
            }

            std::expected<std::size_t, TarError> read(std::span<std::byte> target) override
            {
                if (target.empty() || eof_)
                    return std::size_t{0u};

                stream_.next_out = reinterpret_cast<std::uint8_t*>(target.data());
                stream_.avail_out = target.size();

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
                                makeError(TarErrorCode::IoError, "xz input read failed", errno)
                            );
                        if (fetched == 0u)
                            inputEof_ = true;
                        stream_.next_in = reinterpret_cast<std::uint8_t*>(scratch_.data());
                        stream_.avail_in = fetched;
                    }

                    const lzma_action action = inputEof_ ? LZMA_FINISH : LZMA_RUN;
                    const lzma_ret status = lzma_code(&stream_, action);
                    if (status == LZMA_STREAM_END)
                    {
                        eof_ = true;
                        break;
                    }
                    if (status != LZMA_OK)
                        return std::unexpected(makeError(
                            TarErrorCode::CompressionLibraryError,
                            "lzma_code failed",
                            static_cast<int>(status)
                        ));
                }
                return target.size() - stream_.avail_out;
            }

            bool eof() const noexcept override { return eof_; }

          private:
            std::ifstream input_;
            lzma_stream stream_ = LZMA_STREAM_INIT;
            std::array<std::byte, scratchBufferSize> scratch_{};
            bool initialised_{false};
            bool inputEof_{false};
            bool eof_{false};
        };
    }

    std::expected<std::unique_ptr<ByteSink>, TarError>
    makeXzSink(std::unique_ptr<ByteSink> downstream, CompressionOptions const& options)
    {
        std::uint32_t preset = static_cast<std::uint32_t>(options.xzPreset.value_or(6));
        if (options.xzExtreme)
            preset |= LZMA_PRESET_EXTREME;
        auto sink = std::make_unique<XzSink>(std::move(downstream));
        const auto initialised = sink->initialise(preset);
        if (!initialised)
            return std::unexpected(initialised.error());
        return sink;
    }

    std::expected<std::unique_ptr<ByteSource>, TarError> makeXzSource(std::ifstream input)
    {
        auto source = std::make_unique<XzSource>(std::move(input));
        const auto initialised = source->initialise();
        if (!initialised)
            return std::unexpected(initialised.error());
        return source;
    }
}

#endif
