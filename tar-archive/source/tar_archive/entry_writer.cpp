#include <tar_archive/entry_writer.hpp>
#include <tar_archive/writer.hpp>

#include <utility>

namespace TarArchive
{
    EntryWriter::EntryWriter(Writer* owner, std::uint64_t declaredSize) noexcept
        : owner_{owner}
        , declaredSize_{declaredSize}
        , bytesWritten_{0u}
        , closed_{false}
    {}

    EntryWriter::EntryWriter(EntryWriter&& other) noexcept
        : owner_{std::exchange(other.owner_, nullptr)}
        , declaredSize_{other.declaredSize_}
        , bytesWritten_{other.bytesWritten_}
        , closed_{std::exchange(other.closed_, true)}
    {}

    EntryWriter& EntryWriter::operator=(EntryWriter&& other) noexcept
    {
        if (this != &other)
        {
            if (!closed_ && owner_ != nullptr)
                owner_->abandonActiveEntry(declaredSize_, bytesWritten_);
            owner_ = std::exchange(other.owner_, nullptr);
            declaredSize_ = other.declaredSize_;
            bytesWritten_ = other.bytesWritten_;
            closed_ = std::exchange(other.closed_, true);
        }
        return *this;
    }

    EntryWriter::~EntryWriter()
    {
        if (!closed_ && owner_ != nullptr)
            owner_->abandonActiveEntry(declaredSize_, bytesWritten_);
    }

    std::expected<void, TarError> EntryWriter::write(std::span<std::byte const> bytes)
    {
        if (closed_ || owner_ == nullptr)
            return std::unexpected(
                makeError(TarErrorCode::AlreadyClosed, "EntryWriter has already been closed")
            );
        if (bytesWritten_ + bytes.size() > declaredSize_)
            return std::unexpected(makeError(
                TarErrorCode::OverrunOnWrite,
                "write would exceed declared entry size"
            ));
        if (bytes.empty())
            return {};
        const auto written = owner_->writeEntryPayload(bytes);
        if (!written)
            return std::unexpected(written.error());
        bytesWritten_ += bytes.size();
        return {};
    }

    std::expected<void, TarError> EntryWriter::close() &&
    {
        if (closed_ || owner_ == nullptr)
            return std::unexpected(
                makeError(TarErrorCode::AlreadyClosed, "EntryWriter has already been closed")
            );
        const auto result = owner_->closeActiveEntry(declaredSize_, bytesWritten_);
        closed_ = true;
        owner_ = nullptr;
        return result;
    }
}
