#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace FileTransferProtocol
{
    /**
     * @brief Follows the TLS records of what a server sent and keeps the bytes from the first that begins none.
     *        vsftpd writes its fatal errors into an established TLS connection in plaintext, and a read that
     *        brings them may bring records before them, such as session tickets.
     */
    class TlsRecordTrail
    {
      public:
        /** @brief Enough for the line a server ends a connection with. */
        static constexpr std::size_t keptBytes = 512u;

        /**
         * @brief Takes the next bytes the server sent.
         */
        void feed(std::string_view bytes);

        /**
         * @brief The bytes from the first that begins no TLS record, at most keptBytes. Empty while everything was
         *        records or a part of one.
         */
        std::string_view outsideRecords() const;

      private:
        /** @brief Whether the header bytes so far may begin a record. */
        bool headerMayBeginRecord() const;

        /** @brief The part of a record header read so far. */
        std::string header_{};
        /** @brief The bytes of the current record still to come. */
        std::size_t recordLeft_{0u};
        bool leftRecords_{false};
        std::string outside_{};
    };
}
