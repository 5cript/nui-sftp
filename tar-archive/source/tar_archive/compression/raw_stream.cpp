#include "../compression_internal.hpp"

#include <tar_archive/compression.hpp>
#include <tar_archive/error.hpp>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <utility>

namespace TarArchive::Detail
{
    std::expected<std::ofstream, TarError> openOutputFile(std::filesystem::path const& path)
    {
        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        if (!output.is_open())
            return std::unexpected(makeError(
                TarErrorCode::IoError,
                "could not open '" + path.string() + "' for writing",
                errno
            ));
        return output;
    }

    std::expected<std::ifstream, TarError> openInputFile(std::filesystem::path const& path)
    {
        if (!std::filesystem::exists(path))
            return std::unexpected(makeError(
                TarErrorCode::FileNotFound, "archive does not exist: " + path.string()
            ));
        std::ifstream input{path, std::ios::binary};
        if (!input.is_open())
            return std::unexpected(makeError(
                TarErrorCode::IoError,
                "could not open '" + path.string() + "' for reading",
                errno
            ));
        return input;
    }
}

namespace TarArchive::Detail
{
    namespace
    {
        class RawSink final : public ByteSink
        {
          public:
            explicit RawSink(std::ofstream output) noexcept
                : output_{std::move(output)}
            {}

            ~RawSink() override = default;

            std::expected<void, TarError> write(std::span<std::byte const> bytes) override
            {
                if (bytes.empty())
                    return {};
                output_.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!output_)
                    return std::unexpected(makeError(
                        TarErrorCode::IoError, "underlying ofstream write failed", errno
                    ));
                return {};
            }

            std::expected<void, TarError> finish() override
            {
                output_.flush();
                if (!output_)
                    return std::unexpected(makeError(
                        TarErrorCode::IoError, "flush failed", errno
                    ));
                output_.close();
                return {};
            }

          private:
            std::ofstream output_;
        };

        class RawSource final : public ByteSource
        {
          public:
            explicit RawSource(std::ifstream input) noexcept
                : input_{std::move(input)}
            {}

            ~RawSource() override = default;

            std::expected<std::size_t, TarError> read(std::span<std::byte> target) override
            {
                if (target.empty())
                    return std::size_t{0u};
                input_.read(
                    reinterpret_cast<char*>(target.data()), static_cast<std::streamsize>(target.size())
                );
                const auto produced = static_cast<std::size_t>(input_.gcount());
                if (input_.bad())
                    return std::unexpected(makeError(TarErrorCode::IoError, "read failed", errno));
                if (produced == 0u)
                    eof_ = true;
                return produced;
            }

            bool eof() const noexcept override { return eof_; }

          private:
            std::ifstream input_;
            bool eof_{false};
        };
    }

    std::unique_ptr<ByteSink> makeRawSink(std::ofstream output)
    {
        return std::make_unique<RawSink>(std::move(output));
    }

    std::unique_ptr<ByteSource> makeRawSource(std::ifstream input)
    {
        return std::make_unique<RawSource>(std::move(input));
    }
}
