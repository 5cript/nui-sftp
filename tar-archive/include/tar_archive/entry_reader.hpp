#pragma once

#include <tar_archive/error.hpp>
#include <shared_data/directory_entry.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace TarArchive
{
    class Reader;

    /**
     * @brief Move-only handle representing an entry pulled from a tar Reader.
     *
     * Returned (inside std::optional) by Reader::nextEntry. Holds the parsed metadata
     * for one entry and lets the caller stream the payload. The owning Reader refuses
     * to hand out the next entry until this one is drained to EOF or skip()'d.
     *
     * Lifecycle: Reader::nextEntry consumes the 512-byte USTAR record (plus any leading
     * PAX extended-header records) from the decompressed byte stream before constructing
     * the EntryReader. By the time the caller receives the handle, directoryEntry() is
     * fully populated and the underlying stream is positioned at the first payload byte.
     * No further stream I/O happens until the caller calls read() or skip().
     */
    class EntryReader
    {
      public:
        EntryReader(EntryReader const&) = delete;
        EntryReader& operator=(EntryReader const&) = delete;

        EntryReader(EntryReader&& other) noexcept;
        EntryReader& operator=(EntryReader&& other) noexcept;

        ~EntryReader();

        /**
         * @brief Metadata of this entry (path, type, size, permissions, etc).
         *
         * Available immediately on handle receipt; populated from the header records that
         * Reader::nextEntry already consumed before returning the handle.
         */
        SharedData::DirectoryEntry const& directoryEntry() const noexcept { return meta_; }

        /**
         * @brief Read up to buf.size() bytes of this entry's payload.
         *
         * Returns the number of bytes actually read; a return of 0 indicates end-of-entry
         * and subsequent reads return ReadAfterEnd. After the entry is drained the caller
         * may call Reader::nextEntry() directly; skip() is unnecessary.
         */
        std::expected<std::size_t, TarError> read(std::span<std::byte> buf);

        /**
         * @brief Discard any remaining payload bytes and the 512-byte padding, so the next
         * nextEntry() call succeeds. Safe to call on an already-drained entry.
         */
        std::expected<void, TarError> skip();

        /** @brief Declared total payload size, taken from the entry header. */
        std::uint64_t size() const noexcept { return size_; }

        /** @brief Number of payload bytes the caller has consumed so far. */
        std::uint64_t bytesRead() const noexcept { return bytesRead_; }

        /** @brief True once the entry's payload has been fully read or skipped. */
        bool drained() const noexcept { return drained_; }

      private:
        friend class Reader;

        EntryReader(Reader* owner, SharedData::DirectoryEntry meta, std::uint64_t size) noexcept;

        Reader* owner_{nullptr};
        SharedData::DirectoryEntry meta_{};
        std::uint64_t size_{0};
        std::uint64_t bytesRead_{0};
        bool drained_{true};
    };
}
