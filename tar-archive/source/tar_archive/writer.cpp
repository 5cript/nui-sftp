#include <tar_archive/writer.hpp>
#include <tar_archive/header.hpp>

#include "compression_internal.hpp"

#include <array>
#include <utility>

namespace TarArchive
{
    struct Writer::Implementation
    {
        std::unique_ptr<ByteSink> sink;
        bool hasActiveEntry{false};
        bool finalized{false};
    };

    namespace
    {
        /**
         * @brief Write a run of zero bytes to the sink.
         */
        std::expected<void, TarError> writeZeros(ByteSink& sink, std::size_t count)
        {
            if (count == 0u)
                return {};
            std::array<std::byte, 512u> zeros{};
            std::size_t remaining = count;
            while (remaining > 0u)
            {
                const std::size_t portion = std::min(remaining, zeros.size());
                const auto written =
                    sink.write(std::span<std::byte const>{zeros.data(), portion});
                if (!written)
                    return std::unexpected(written.error());
                remaining -= portion;
            }
            return {};
        }

        /**
         * @brief Zero-pad the sink up to the next 512-byte record boundary based on how
         * many payload bytes were already emitted.
         */
        std::expected<void, TarError> padToBoundary(ByteSink& sink, std::uint64_t bytesEmitted)
        {
            const std::size_t modulus =
                static_cast<std::size_t>(bytesEmitted % recordSize);
            if (modulus == 0u)
                return {};
            return writeZeros(sink, recordSize - modulus);
        }
    }

    Writer::Writer(std::unique_ptr<Implementation> implementation) noexcept
        : impl_{std::move(implementation)}
    {}

    Writer Writer::makeFromSink(std::unique_ptr<ByteSink> sink)
    {
        auto implementation = std::make_unique<Implementation>();
        implementation->sink = std::move(sink);
        return Writer{std::move(implementation)};
    }

    Writer::Writer(Writer&&) noexcept = default;
    Writer& Writer::operator=(Writer&&) noexcept = default;

    Writer::~Writer()
    {
        if (impl_ == nullptr)
            return;
        if (!impl_->finalized)
            (void)finalize();
    }

    bool Writer::isFinalized() const noexcept
    {
        return impl_ == nullptr || impl_->finalized;
    }

    std::expected<EntryWriter, TarError>
    Writer::beginEntry(SharedData::DirectoryEntry const& meta)
    {
        if (impl_ == nullptr || impl_->finalized)
            return std::unexpected(
                makeError(TarErrorCode::WriteAfterFinalize, "writer has already been finalised")
            );
        if (impl_->hasActiveEntry)
            return std::unexpected(
                makeError(TarErrorCode::EntryStillOpen, "previous entry has not been closed")
            );

        auto built = buildRecords(meta);
        if (!built)
            return std::unexpected(built.error());

        for (auto const& record : built->records)
        {
            const auto written = impl_->sink->write(std::span<std::byte const>{record});
            if (!written)
                return std::unexpected(written.error());
        }

        impl_->hasActiveEntry = true;
        return EntryWriter{this, built->payloadSize};
    }

    std::expected<void, TarError> Writer::finalize()
    {
        if (impl_ == nullptr)
            return std::unexpected(
                makeError(TarErrorCode::AlreadyFinalized, "writer is inert (moved-from or already finalised)")
            );
        if (impl_->finalized)
            return std::unexpected(
                makeError(TarErrorCode::AlreadyFinalized, "writer has already been finalised")
            );
        if (impl_->hasActiveEntry)
            return std::unexpected(
                makeError(TarErrorCode::EntryStillOpen, "active entry must be closed before finalising")
            );

        const auto trailer = writeZeros(*impl_->sink, recordSize * 2u);
        if (!trailer)
            return std::unexpected(trailer.error());

        const auto finished = impl_->sink->finish();
        if (!finished)
            return std::unexpected(finished.error());

        impl_->finalized = true;
        return {};
    }

    std::expected<void, TarError> Writer::writeEntryPayload(std::span<std::byte const> bytes)
    {
        if (impl_ == nullptr)
            return std::unexpected(
                makeError(TarErrorCode::WriteAfterFinalize, "writer is inert")
            );
        return impl_->sink->write(bytes);
    }

    std::expected<void, TarError>
    Writer::closeActiveEntry(std::uint64_t declared, std::uint64_t written)
    {
        if (impl_ == nullptr)
            return std::unexpected(
                makeError(TarErrorCode::WriteAfterFinalize, "writer is inert")
            );
        if (!impl_->hasActiveEntry)
            return std::unexpected(
                makeError(TarErrorCode::NoActiveEntry, "no active entry to close")
            );

        if (written != declared)
        {
            impl_->hasActiveEntry = false;
            const std::uint64_t missing = declared - written;
            const auto filled = writeZeros(*impl_->sink, missing);
            if (filled)
                (void)padToBoundary(*impl_->sink, declared);
            return std::unexpected(makeError(
                TarErrorCode::UnderrunOnClose,
                "entry closed with fewer bytes than declared"
            ));
        }

        const auto padded = padToBoundary(*impl_->sink, declared);
        impl_->hasActiveEntry = false;
        if (!padded)
            return std::unexpected(padded.error());
        return {};
    }

    void Writer::abandonActiveEntry(std::uint64_t declared, std::uint64_t written) noexcept
    {
        if (impl_ == nullptr || !impl_->hasActiveEntry)
            return;
        try
        {
            if (written < declared)
                (void)writeZeros(*impl_->sink, declared - written);
            (void)padToBoundary(*impl_->sink, declared);
        }
        catch (...)
        {
        }
        impl_->hasActiveEntry = false;
    }
}
