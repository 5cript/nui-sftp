#pragma once

#include <ftp/client/control_connection.hpp>
#include <ftp/client/data_connection.hpp>
#include <ftp/client/tls.hpp>
#include <ftp/protocol/reply.hpp>
#include <remote_io/remote_error.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace FileTransferProtocol
{
    /**
     * @brief What a transfer takes from its session: the logged-in control connection, the passive choice its
     *        connections share, the TLS settings of the login when the data is protected (PROT P).
     */
    struct TransferSession
    {
        std::shared_ptr<ControlConnection> control{};
        std::shared_ptr<PassiveChoice> passiveChoice{};
        std::optional<TlsConnectionSettings> tls{};
    };

    struct TransferOptions
    {
        DataConnectionOptions data{};
        /** @brief From the end of the data to the final reply. */
        std::chrono::milliseconds finalReplyTimeout{15'000};
    };

    struct TransferResult
    {
        /** @brief Bytes of this transfer, without the offset it started at. */
        std::uint64_t bytes{0u};
        /**
         * @brief Nothing when no final reply came in time and the size showed the data complete. The control
         *        connection is Dead then, its replies are out of step.
         */
        std::optional<Reply> finalReply{};
    };

    struct DownloadRequest
    {
        std::string path{};
        /** @brief Starts here with REST (RFC 3659 5), 0 for the whole file. */
        std::uint64_t offset{0u};
        /** @brief The size of the whole file when the caller knows it, to tell a complete download from a cut one. */
        std::optional<std::uint64_t> expectedSize{};
    };

    /**
     * @brief One RETR on its own data connection, on the strand of the control connection. Completion takes the end
     *        of the data and a positive final reply, in either order. Data that ended earlier than the expected size
     *        is checked again with SIZE: the same size now means the file changed, else Truncated.
     */
    class Download
    {
        struct Private
        {};

      public:
        /**
         * @brief Opens the data connection, sends REST when the offset is not 0 and RETR, and waits for the first
         *        reply. With TLS the handshake runs next to that wait, servers start it only once RETR came.
         *
         * @return The running download. The replyError() of a refused REST or RETR, NoSecureConnection when the data
         *         connection gets no TLS, also when the server requires session reuse and the connection resumed
         *         nothing. The control connection stays usable after a refused RETR.
         */
        static boost::asio::awaitable<std::expected<std::shared_ptr<Download>, RemoteIo::RemoteError>>
        start(TransferSession session, DownloadRequest request, TransferOptions options = {});

        Download(
            Private,
            TransferSession session,
            DownloadRequest request,
            TransferOptions options,
            std::shared_ptr<DataConnection> data,
            std::shared_ptr<PendingCommand> retrieve
        );
        ~Download();
        Download(Download const&) = delete;
        Download& operator=(Download const&) = delete;
        Download(Download&&) = delete;
        Download& operator=(Download&&) = delete;

        /**
         * @brief The next bytes, 0 once the data ended, then finish() tells whether the download is complete.
         *
         * @return The data connection's error, after which the transfer is aborted with ABOR.
         */
        boost::asio::awaitable<std::expected<std::size_t, RemoteIo::RemoteError>>
        readSome(boost::asio::mutable_buffer buffer);

        /**
         * @brief The final reply and the size checks, once readSome() returned 0.
         *
         * @return The result. The replyError() of a negative final reply, Truncated for data that ended early or
         *         without close_notify and the wrong size, TimedOut when no final reply came and the size proves
         *         nothing, Failure before the data ended.
         */
        boost::asio::awaitable<std::expected<TransferResult, RemoteIo::RemoteError>> finish();

        /**
         * @brief The size the first reply announced, when the download starts at 0.
         */
        std::optional<std::uint64_t> announcedSize() const;

      private:
        /**
         * @brief Bytes this transfer should bring, from the caller or the announcement.
         */
        std::optional<std::uint64_t> expectedBytes() const;
        boost::asio::awaitable<std::expected<TransferResult, RemoteIo::RemoteError>> checkSize(Reply finalReply);
        void abort();

        TransferSession session_;
        DownloadRequest request_;
        TransferOptions options_;
        std::shared_ptr<DataConnection> data_;
        std::shared_ptr<PendingCommand> retrieve_;
        std::optional<std::uint64_t> announced_{};
        /** @brief A final reply that came before the end of the data. */
        std::optional<Reply> earlyFinalReply_{};
        std::uint64_t bytes_{0u};
        bool dataEnded_{false};
        bool aborted_{false};
    };

    enum class UploadMode : std::uint8_t
    {
        /** @brief STOR, with REST before it to continue at an offset. */
        Store,
        /** @brief APPE, adds to the end of the file. */
        Append,
    };

    struct UploadRequest
    {
        std::string path{};
        UploadMode mode{UploadMode::Store};
        /** @brief REST before STOR, to continue a partial file (RFC 3659 5). Not used with Append. */
        std::uint64_t offset{0u};
    };

    struct UploadResult
    {
        /** @brief Bytes of this transfer, without the offset it started at. */
        std::uint64_t bytes{0u};
        /**
         * @brief Nothing when no final reply came in time. The control connection is Dead then, and the upload is
         *        complete only when SIZE on another connection shows the offset plus these bytes.
         */
        std::optional<Reply> finalReply{};
    };

    /**
     * @brief One STOR or APPE on its own data connection, on the strand of the control connection. The data ends so
     *        that the server sees all of it before the final reply counts. A failed upload resets its data connection:
     *        a FIN could make the server take the cut file for a complete one.
     */
    class Upload
    {
        struct Private
        {};

      public:
        /**
         * @brief Opens the data connection, sends REST for a Store at an offset, STOR or APPE, and waits for the first
         *        reply, the TLS handshake beside it as for Download::start().
         *
         * @return The running upload, errors as for Download::start().
         */
        static boost::asio::awaitable<std::expected<std::shared_ptr<Upload>, RemoteIo::RemoteError>>
        start(TransferSession session, UploadRequest request, TransferOptions options = {});

        Upload(
            Private,
            TransferSession session,
            UploadRequest request,
            TransferOptions options,
            std::shared_ptr<DataConnection> data,
            std::shared_ptr<PendingCommand> store
        );
        ~Upload();
        Upload(Upload const&) = delete;
        Upload& operator=(Upload const&) = delete;
        Upload(Upload&&) = delete;
        Upload& operator=(Upload&&) = delete;

        /**
         * @brief Writes all of @p buffer.
         *
         * @return The replyError() of a server that ended the transfer meanwhile, a full disk for one, else the data
         *         connection's error. The upload is aborted then.
         */
        boost::asio::awaitable<std::expected<void, RemoteIo::RemoteError>> write(boost::asio::const_buffer buffer);

        /**
         * @brief Ends the data so the server takes all of it, then the final reply.
         *
         * @return The result. The replyError() of a negative final reply, the error of a data end that failed.
         */
        boost::asio::awaitable<std::expected<UploadResult, RemoteIo::RemoteError>> finish();

      private:
        /**
         * @brief What a failed write or end of the data means: the server's refusal when it ended the transfer, else
         *        @p dataError after an abort.
         */
        boost::asio::awaitable<RemoteIo::RemoteError> failure(RemoteIo::RemoteError dataError);
        Verb verb() const;
        void abort();

        TransferSession session_;
        UploadRequest request_;
        TransferOptions options_;
        std::shared_ptr<DataConnection> data_;
        std::shared_ptr<PendingCommand> store_;
        std::optional<Reply> earlyFinalReply_{};
        std::uint64_t bytes_{0u};
        bool aborted_{false};
    };
}
