#include <tar_archive/entry_reader.hpp>
#include <tar_archive/reader.hpp>

#include <utility>

namespace TarArchive
{
    EntryReader::EntryReader(Reader* owner, SharedData::DirectoryEntry meta, std::uint64_t size) noexcept
        : owner_{owner}
        , meta_{std::move(meta)}
        , size_{size}
        , bytesRead_{0u}
        , drained_{false}
    {}

    EntryReader::EntryReader(EntryReader&& other) noexcept
        : owner_{std::exchange(other.owner_, nullptr)}
        , meta_{std::move(other.meta_)}
        , size_{other.size_}
        , bytesRead_{other.bytesRead_}
        , drained_{std::exchange(other.drained_, true)}
    {}

    EntryReader& EntryReader::operator=(EntryReader&& other) noexcept
    {
        if (this != &other)
        {
            if (!drained_ && owner_ != nullptr)
                (void)owner_->skipRemaining();
            owner_ = std::exchange(other.owner_, nullptr);
            meta_ = std::move(other.meta_);
            size_ = other.size_;
            bytesRead_ = other.bytesRead_;
            drained_ = std::exchange(other.drained_, true);
        }
        return *this;
    }

    EntryReader::~EntryReader()
    {
        if (!drained_ && owner_ != nullptr)
            (void)owner_->skipRemaining();
    }

    std::expected<std::size_t, TarError> EntryReader::read(std::span<std::byte> buf)
    {
        if (drained_)
            return std::size_t{0u};
        if (owner_ == nullptr)
            return std::unexpected(
                makeError(TarErrorCode::ReadAfterEnd, "EntryReader is detached from its Reader")
            );
        if (bytesRead_ >= size_)
        {
            drained_ = true;
            owner_->detachActiveEntry();
            owner_ = nullptr;
            return std::size_t{0u};
        }

        const std::uint64_t remaining = size_ - bytesRead_;
        const std::size_t maxReadable = static_cast<std::size_t>(
            std::min<std::uint64_t>(buf.size(), remaining)
        );
        const auto produced =
            owner_->readEntryPayload(buf.first(maxReadable));
        if (!produced)
            return std::unexpected(produced.error());
        bytesRead_ += *produced;

        if (bytesRead_ == size_)
        {
            const auto padded = owner_->skipRemaining();
            if (!padded)
                return std::unexpected(padded.error());
            drained_ = true;
            owner_ = nullptr;
        }
        return *produced;
    }

    std::expected<void, TarError> EntryReader::skip()
    {
        if (drained_ || owner_ == nullptr)
            return {};
        const auto skipped = owner_->skipRemaining();
        drained_ = true;
        owner_ = nullptr;
        if (!skipped)
            return std::unexpected(skipped.error());
        return {};
    }
}
