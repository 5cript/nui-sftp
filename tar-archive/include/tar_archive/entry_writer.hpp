#pragma once

#include <tar_archive/error.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace TarArchive
{
    class Writer;

    /**
     * @brief Move-only handle representing an in-progress entry in a tar Writer.
     *
     * Returned by Writer::beginEntry. The caller streams the entry payload into this
     * handle via write(), then finalises it with close(). The Writer that produced the
     * handle rejects beginEntry() while an EntryWriter is still alive.
     *
     * If the handle is destroyed without a successful close(), the entry is marked as
     * aborted: its 512-byte alignment padding is still written so the archive remains
     * navigable, but the caller cannot observe a close error in that path.
     */
    class EntryWriter
    {
      public:
        EntryWriter(EntryWriter const&) = delete;
        EntryWriter& operator=(EntryWriter const&) = delete;

        EntryWriter(EntryWriter&& other) noexcept;
        EntryWriter& operator=(EntryWriter&& other) noexcept;

        ~EntryWriter();

        /**
         * @brief Append bytes to the entry payload.
         *
         * Writing more bytes than the entry's declared size returns OverrunOnWrite; no
         * partial success in that case. After close() (or move-from) the handle is spent
         * and further writes return AlreadyClosed.
         */
        std::expected<void, TarError> write(std::span<std::byte const> bytes);

        /**
         * @brief Finalise the entry. Pads the payload to the 512-byte boundary and releases
         * the single-active-entry guard on the owning Writer. Must be called on an rvalue
         * handle (`std::move(handle).close()`); the handle is spent afterwards.
         *
         * Returns UnderrunOnClose if the bytes written do not match the declared size.
         */
        std::expected<void, TarError> close() &&;

        /** @brief Declared total payload size of this entry in bytes. */
        std::uint64_t declaredSize() const noexcept { return declaredSize_; }

        /** @brief Number of payload bytes successfully written so far. */
        std::uint64_t bytesWritten() const noexcept { return bytesWritten_; }

      private:
        friend class Writer;

        EntryWriter(Writer* owner, std::uint64_t declaredSize) noexcept;

        Writer* owner_{nullptr};
        std::uint64_t declaredSize_{0};
        std::uint64_t bytesWritten_{0};
        bool closed_{true};
    };
}
