#include <tar_archive/reader.hpp>
#include <tar_archive/header.hpp>

#include "compression_internal.hpp"

#include <array>
#include <cstring>
#include <utility>
#include <vector>

namespace TarArchive
{
    struct Reader::Implementation
    {
        std::unique_ptr<ByteSource> source;
        bool hasActiveEntry{false};
        std::uint64_t activeDeclaredSize{0u};
        std::uint64_t activeBytesRead{0u};
        bool endOfArchive{false};
    };

    namespace
    {
        /**
         * @brief Read exactly @p target.size() bytes from @p source. Short reads are looped
         * until either the buffer is full or EOF is reached.
         */
        std::expected<std::size_t, TarError>
        readFull(ByteSource& source, std::span<std::byte> target)
        {
            std::size_t total = 0u;
            while (total < target.size())
            {
                const auto produced = source.read(target.subspan(total));
                if (!produced)
                    return std::unexpected(produced.error());
                if (*produced == 0u)
                    break;
                total += *produced;
            }
            return total;
        }

        std::expected<RawRecord, TarError> readRecord(ByteSource& source)
        {
            RawRecord record{};
            const auto got =
                readFull(source, std::span<std::byte>{record.data(), record.size()});
            if (!got)
                return std::unexpected(got.error());
            if (*got == 0u)
                return std::unexpected(
                    makeError(TarErrorCode::TruncatedArchive, "unexpected end-of-archive")
                );
            if (*got != record.size())
                return std::unexpected(makeError(
                    TarErrorCode::TruncatedArchive, "partial tar record at end of stream"
                ));
            return record;
        }

        /**
         * @brief Consume @p count bytes from @p source, discarding them. Used for padding
         * after entry payloads.
         */
        std::expected<void, TarError>
        discardBytes(ByteSource& source, std::uint64_t count)
        {
            if (count == 0u)
                return {};
            std::array<std::byte, 4096u> scratch{};
            std::uint64_t remaining = count;
            while (remaining > 0u)
            {
                const std::size_t chunk = static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining, scratch.size())
                );
                const auto got =
                    readFull(source, std::span<std::byte>{scratch.data(), chunk});
                if (!got)
                    return std::unexpected(got.error());
                if (*got != chunk)
                    return std::unexpected(
                        makeError(TarErrorCode::TruncatedArchive, "archive ended mid-padding")
                    );
                remaining -= chunk;
            }
            return {};
        }

        /**
         * @brief Read @p size payload bytes plus padding to next 512-byte boundary.
         */
        std::expected<std::vector<std::byte>, TarError>
        readPayloadWithPadding(ByteSource& source, std::uint64_t size)
        {
            std::vector<std::byte> payload(static_cast<std::size_t>(size));
            if (size > 0u)
            {
                const auto got = readFull(source, std::span<std::byte>{payload});
                if (!got)
                    return std::unexpected(got.error());
                if (*got != payload.size())
                    return std::unexpected(makeError(
                        TarErrorCode::TruncatedArchive, "archive ended mid-entry payload"
                    ));
            }
            const std::uint64_t modulus = size % recordSize;
            if (modulus != 0u)
            {
                const auto padded = discardBytes(source, recordSize - modulus);
                if (!padded)
                    return std::unexpected(padded.error());
            }
            return payload;
        }
    }

    Reader::Reader(std::unique_ptr<Implementation> implementation) noexcept
        : impl_{std::move(implementation)}
    {}

    Reader Reader::makeFromSource(std::unique_ptr<ByteSource> source)
    {
        auto implementation = std::make_unique<Implementation>();
        implementation->source = std::move(source);
        return Reader{std::move(implementation)};
    }

    Reader::Reader(Reader&&) noexcept = default;
    Reader& Reader::operator=(Reader&&) noexcept = default;
    Reader::~Reader() = default;

    std::expected<std::optional<EntryReader>, TarError> Reader::nextEntry()
    {
        if (impl_ == nullptr)
            return std::unexpected(
                makeError(TarErrorCode::ReadAfterEnd, "reader is inert")
            );
        if (impl_->hasActiveEntry)
            return std::unexpected(
                makeError(TarErrorCode::EntryStillOpen, "previous entry has not been drained or skipped")
            );
        if (impl_->endOfArchive)
            return std::optional<EntryReader>{};

        PaxOverrides overrides;

        while (true)
        {
            auto recordOrError = readRecord(*impl_->source);
            if (!recordOrError)
                return std::unexpected(recordOrError.error());
            const RawRecord record = *recordOrError;

            if (isZeroRecord(record))
            {
                auto secondOrError = readRecord(*impl_->source);
                if (!secondOrError)
                    return std::unexpected(secondOrError.error());
                if (isZeroRecord(*secondOrError))
                {
                    impl_->endOfArchive = true;
                    return std::optional<EntryReader>{};
                }
                return std::unexpected(makeError(
                    TarErrorCode::InvalidHeader,
                    "single zero record not followed by another zero record"
                ));
            }

            auto parsed = parseRecord(record);
            if (!parsed)
                return std::unexpected(parsed.error());

            if (parsed->typeflag == TypeFlag::ExtendedHeader ||
                parsed->typeflag == TypeFlag::GlobalExtendedHeader)
            {
                auto payload = readPayloadWithPadding(*impl_->source, parsed->size);
                if (!payload)
                    return std::unexpected(payload.error());
                auto parsedOverrides =
                    parsePaxPayload(std::span<std::byte const>{*payload});
                if (!parsedOverrides)
                    return std::unexpected(parsedOverrides.error());
                if (parsed->typeflag == TypeFlag::ExtendedHeader)
                    overrides = *parsedOverrides;
                continue;
            }

            SharedData::DirectoryEntry meta =
                assembleDirectoryEntry(*parsed, overrides);

            const std::uint64_t payloadSize =
                (parsed->typeflag == TypeFlag::Directory ||
                 parsed->typeflag == TypeFlag::SymbolicLink ||
                 parsed->typeflag == TypeFlag::HardLink ||
                 parsed->typeflag == TypeFlag::CharacterSpecial ||
                 parsed->typeflag == TypeFlag::BlockSpecial ||
                 parsed->typeflag == TypeFlag::FifoSpecial)
                    ? 0u
                    : meta.size;

            impl_->hasActiveEntry = true;
            impl_->activeDeclaredSize = payloadSize;
            impl_->activeBytesRead = 0u;

            return std::optional<EntryReader>{
                EntryReader{this, std::move(meta), payloadSize}
            };
        }
    }

    std::expected<std::size_t, TarError> Reader::readEntryPayload(std::span<std::byte> buf)
    {
        if (impl_ == nullptr)
            return std::unexpected(
                makeError(TarErrorCode::ReadAfterEnd, "reader is inert")
            );
        if (!impl_->hasActiveEntry)
            return std::unexpected(
                makeError(TarErrorCode::NoActiveEntry, "no active entry to read from")
            );
        if (buf.empty())
            return std::size_t{0u};

        const auto produced = impl_->source->read(buf);
        if (!produced)
            return std::unexpected(produced.error());
        impl_->activeBytesRead += *produced;
        return *produced;
    }

    std::expected<void, TarError> Reader::skipRemaining()
    {
        if (impl_ == nullptr)
            return {};
        if (!impl_->hasActiveEntry)
            return {};

        const std::uint64_t remaining =
            impl_->activeDeclaredSize - impl_->activeBytesRead;
        const auto skipped = discardBytes(*impl_->source, remaining);
        if (!skipped)
        {
            impl_->hasActiveEntry = false;
            return std::unexpected(skipped.error());
        }

        const std::uint64_t modulus = impl_->activeDeclaredSize % recordSize;
        if (modulus != 0u)
        {
            const auto padded = discardBytes(*impl_->source, recordSize - modulus);
            if (!padded)
            {
                impl_->hasActiveEntry = false;
                return std::unexpected(padded.error());
            }
        }
        impl_->hasActiveEntry = false;
        return {};
    }

    void Reader::detachActiveEntry() noexcept
    {
        if (impl_ == nullptr)
            return;
        impl_->hasActiveEntry = false;
    }
}
