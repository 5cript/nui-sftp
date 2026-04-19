#pragma once

#include <tar_archive/compression.hpp>
#include <tar_archive/entry_writer.hpp>
#include <tar_archive/error.hpp>
#include <shared_data/directory_entry.hpp>

#include <cstdint>
#include <expected>
#include <memory>

namespace TarArchive
{
    class Archive;

    /**
     * @brief Streaming tar archive writer.
     *
     * Produced exclusively by Archive::openWriter. Writes one entry at a time via
     * beginEntry(); only one EntryWriter can be alive at a time per Writer. Call
     * finalize() to flush compression buffers and emit the two trailing zero records;
     * the destructor also performs this flush best-effort if not already finalised.
     *
     * Move-only. Moved-from Writers are inert (all operations return AlreadyFinalized).
     */
    class Writer
    {
      public:
        Writer(Writer const&) = delete;
        Writer& operator=(Writer const&) = delete;

        Writer(Writer&& other) noexcept;
        Writer& operator=(Writer&& other) noexcept;

        ~Writer();

        /**
         * @brief Begin a new entry. Fails with EntryStillOpen if a previous EntryWriter
         * produced by this Writer is still alive, or with WriteAfterFinalize if the Writer
         * has already been finalised.
         */
        std::expected<EntryWriter, TarError> beginEntry(SharedData::DirectoryEntry const& meta);

        /**
         * @brief Flush pending compression buffers, write two trailing zero records and
         * close the underlying byte sink. Idempotent in the sense that a second call
         * returns AlreadyFinalized. Any active EntryWriter must be closed first.
         */
        std::expected<void, TarError> finalize();

        /** @brief True once finalize() has been called successfully. */
        bool isFinalized() const noexcept;

        /**
         * @brief Wrap an externally-provided ByteSink in a Writer.
         *
         * Typically you'll use Archive::openWriter, which owns the sink. This
         * factory exists for callers that need to plug into a sink they already
         * control (e.g. a sink that writes to a foreign stream). Ownership of
         * the sink transfers to the Writer.
         */
        static Writer makeFromSink(std::unique_ptr<ByteSink> sink);

      private:
        friend class Archive;
        friend class EntryWriter;

        struct Implementation;
        std::unique_ptr<Implementation> impl_;

        explicit Writer(std::unique_ptr<Implementation> implementation) noexcept;

        std::expected<void, TarError> writeEntryPayload(std::span<std::byte const> bytes);
        std::expected<void, TarError> closeActiveEntry(std::uint64_t declared, std::uint64_t written);
        void abandonActiveEntry(std::uint64_t declared, std::uint64_t written) noexcept;
    };
}
