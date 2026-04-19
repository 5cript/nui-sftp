#pragma once

#include <tar_archive/compression.hpp>
#include <tar_archive/entry_reader.hpp>
#include <tar_archive/error.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>

namespace TarArchive
{
    class Archive;

    /**
     * @brief Streaming tar archive reader.
     *
     * Produced exclusively by Archive::openReader. Yields entries one at a time via
     * nextEntry(); only one EntryReader can be alive at a time per Reader.
     *
     * When the caller calls nextEntry() while the previous entry's payload has not been
     * fully consumed, the call fails with EntryStillOpen. The caller can recover by
     * calling skip() on the outstanding EntryReader (reading the remaining bytes is also
     * valid).
     *
     * Move-only. Moved-from Readers are inert.
     */
    class Reader
    {
      public:
        Reader(Reader const&) = delete;
        Reader& operator=(Reader const&) = delete;

        Reader(Reader&& other) noexcept;
        Reader& operator=(Reader&& other) noexcept;

        ~Reader();

        /**
         * @brief Pull the next entry from the archive.
         *
         * Reads ahead far enough to fully populate the next EntryReader's metadata
         * (including consuming any leading PAX extended-header records from the
         * decompressed byte stream). Returns std::nullopt wrapped in expected when
         * end-of-archive is reached (i.e. two consecutive zero records were parsed).
         * Returns EntryStillOpen when the previously returned EntryReader was neither
         * drained nor skipped.
         */
        std::expected<std::optional<EntryReader>, TarError> nextEntry();

        /**
         * @brief Wrap an externally-provided ByteSource in a Reader.
         *
         * Typically you'll use Archive::openReader, which owns the source. This
         * factory exists for callers that need to plug into a source they already
         * control. Ownership of the source transfers to the Reader.
         */
        static Reader makeFromSource(std::unique_ptr<ByteSource> source);

      private:
        friend class Archive;
        friend class EntryReader;

        struct Implementation;
        std::unique_ptr<Implementation> impl_;

        explicit Reader(std::unique_ptr<Implementation> implementation) noexcept;

        std::expected<std::size_t, TarError> readEntryPayload(std::span<std::byte> buf);
        std::expected<void, TarError> skipRemaining();
        void detachActiveEntry() noexcept;
    };
}
