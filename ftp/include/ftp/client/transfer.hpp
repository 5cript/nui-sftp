#pragma once

#include <ftp/client/control_connection.hpp>
#include <ftp/client/data_connection.hpp>
#include <ftp/client/tls.hpp>
#include <ftp/protocol/command.hpp>
#include <ftp/protocol/listing.hpp>
#include <ftp/protocol/reply.hpp>
#include <remote_io/remote_error.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace FileTransferProtocol
{
    /**
     * @brief What a transfer takes from its session: the logged-in control connection, the passive choice its
     *        connections share, the TLS settings of the login when the data is protected (PROT P).
     *
     * The control connection is the transfer's alone from start() until finish() returned or the transfer is gone,
     * nothing else may send commands on it meanwhile. EPSV or PASV, REST and the transfer command belong together: a
     * command between them would take the passive port (RFC 1123 4.1.2.6) or the restart offset (RFC 3659 5.3), and
     * a download that continues after a pause sends all of them again from readSome(). Nothing enforces this, the
     * owner of the connection hands it to one transfer at a time.
     */
    struct TransferSession
    {
        std::shared_ptr<ControlConnection> control{};
        std::shared_ptr<PassiveChoice> passiveChoice{};
        std::optional<TlsConnectionSettings> tls{};
        /** @brief The login's features name REST STREAM (RFC 3659 5.4), a transfer may continue at an offset. */
        bool restartStream{false};
    };

    struct TransferOptions
    {
        DataConnectionOptions data{};
        /** @brief From the end of the data to the final reply. */
        std::chrono::milliseconds finalReplyTimeout{15'000};
        /**
         * @brief A download not read for this long drops its data connection and continues with REST on the next
         *        read, before the server's data timeout ends more (IIS after 30 s, vsftpd the whole session). Only
         *        with TransferSession::restartStream, nothing never drops.
         */
        std::optional<std::chrono::milliseconds> pauseDropAfter{std::chrono::seconds{20}};
        /** @brief How long a failed upload waits for the server's final reply, which may come after the data broke. */
        std::chrono::milliseconds refusalWait{1'000};
    };

    struct TransferResult
    {
        /** @brief Bytes of this transfer, without the offset it started or a download was sought at. */
        std::uint64_t bytes{0u};
        /**
         * @brief Nothing when no final reply came in time and the size showed the data complete. The control
         *        connection is Dead then, its replies are out of step.
         */
        std::optional<Reply> finalReply{};
        /** @brief The TLS handshake of the last data connection resumed the control connection's session. */
        bool resumedSession{false};
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
    class Download : public std::enable_shared_from_this<Download>
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
         * @brief The next bytes, 0 once the data ended, then finish() tells whether the download is complete. After a
         *        drop for a pause the transfer continues with REST at the bytes read so far.
         *
         * @return The data connection's error, after which the transfer is aborted with ABOR, or the error of the
         *         new start after a drop.
         */
        boost::asio::awaitable<std::expected<std::size_t, RemoteIo::RemoteError>>
        readSome(boost::asio::mutable_buffer buffer);

        /**
         * @brief The final reply and the size checks, once readSome() returned 0.
         *
         * @return The result. The replyError() of a negative final reply, Truncated for data that ended early or
         *         without close_notify and the wrong size, TimedOut when no final reply came and the size proves
         *         nothing, Failure before the data ended, Cancelled after an abort(), one that came during its waits
         *         included.
         */
        boost::asio::awaitable<std::expected<TransferResult, RemoteIo::RemoteError>> finish();

        /**
         * @brief The size the first reply announced, when the download starts at 0.
         */
        std::optional<std::uint64_t> announcedSize() const;

        /**
         * @brief Ends the download as cancelled: closes the data connection as DataConnection::closeOnceAnswered()
         *        does and sends ABOR, the control connection drains behind it and stays usable. Reads and finish()
         *        fail as Cancelled from then on, a first read under way once the server answered on the data
         *        connection.
         */
        void abort();

        /**
         * @brief Continues the download at @p position of the file. The position it reached costs nothing. Any other
         *        one ends the running transfer like abort(), and the next read starts it again there, with REST unless
         *        @p position is 0, on the same control connection. Not while a read is under way.
         *
         * @return Cancelled after abort().
         */
        std::expected<void, RemoteIo::RemoteError> seek(std::uint64_t position);

      private:
        /**
         * @brief readSome() without its pause bookkeeping: a transfer dropped for a pause starts again first, the end
         *        of the data finishes the data connection.
         */
        boost::asio::awaitable<std::expected<std::size_t, RemoteIo::RemoteError>>
        readData(boost::asio::mutable_buffer buffer);
        /**
         * @brief Bytes this transfer should bring from its offset on, from the caller's size or the announcement.
         */
        std::optional<std::uint64_t> expectedBytes() const;
        boost::asio::awaitable<std::expected<TransferResult, RemoteIo::RemoteError>> checkSize(Reply finalReply);
        /**
         * @brief Starts the transfer again at the bytes read so far, after a drop for a pause or a seek.
         */
        boost::asio::awaitable<std::expected<void, RemoteIo::RemoteError>> resume();
        /**
         * @brief Notes the start or a read and starts the pause timer unless it runs already.
         */
        void armPauseDrop();
        void waitForPause(std::chrono::steady_clock::duration duration);
        /**
         * @brief The pause timer ended: waits on while a read is under way or the last one is recent, else drops.
         */
        void takePause();
        /**
         * @brief Closes the data connection of a paused download and sends ABOR, the login stays.
         */
        void dropForPause();
        /**
         * @brief Closes the data connection and sends ABOR, the next read starts the transfer again.
         */
        void drop(std::string_view why);

        TransferSession session_;
        DownloadRequest request_;
        TransferOptions options_;
        std::shared_ptr<DataConnection> data_;
        std::shared_ptr<PendingCommand> retrieve_;
        std::optional<std::uint64_t> announced_{};
        /** @brief A final reply that came before the end of the data. */
        std::optional<Reply> earlyFinalReply_{};
        std::uint64_t bytes_{0u};
        boost::asio::steady_timer pauseTimer_;
        std::chrono::steady_clock::time_point lastRead_{};
        bool pauseArmed_{false};
        bool reading_{false};
        bool dataEnded_{false};
        bool aborted_{false};
        /** @brief Dropped for a pause or a seek, the next read starts the transfer again. */
        bool dropped_{false};
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
        /** @brief The positive final reply, without it no upload counts as complete. */
        Reply finalReply{};
        /** @brief The TLS handshake of the data connection resumed the control connection's session. */
        bool resumedSession{false};
    };

    /**
     * @brief One STOR or APPE on its own data connection, on the strand of the control connection. The data ends so
     *        that the server sees all of it before the final reply counts. A failed, aborted or dropped upload resets
     *        its data connection: a FIN could make the server take the cut file for a complete one.
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
         * @return The replyError() of a server that ended the transfer meanwhile or does so within the refusal wait,
         *         a full disk for one, else the data connection's error. The upload is aborted then.
         */
        boost::asio::awaitable<std::expected<void, RemoteIo::RemoteError>> write(boost::asio::const_buffer buffer);

        /**
         * @brief Ends the data so the server takes all of it, then the final reply.
         *
         * @return The result. The replyError() of a negative final reply. The error of a data end that failed, unless
         *         a positive final reply within the refusal wait confirms the upload. TimedOut when no final reply
         *         came in time: the control connection is Dead then, and the upload is complete only when SIZE on
         *         another connection shows the offset plus the bytes written.
         */
        boost::asio::awaitable<std::expected<UploadResult, RemoteIo::RemoteError>> finish();

        /**
         * @brief Ends the upload as failed: resets the data connection and sends ABOR, or ends an upload cut before
         *        any byte cleanly, see endData(). A final reply that comes after it never completes the upload, not
         *        even a 226, nor one a running finish() waits for: the server may keep the partial file.
         */
        void abort();

      private:
        /**
         * @brief The final reply behind a failed write or end of the data, when it is there within the refusal wait.
         */
        boost::asio::awaitable<std::optional<Reply>> answerToFailedData();
        /**
         * @brief Ends the upload as failed: with the server's refusal when @p answer is one, else with @p dataError
         *        after an abort.
         */
        RemoteIo::RemoteError failureOf(std::optional<Reply> const& answer, RemoteIo::RemoteError dataError);
        Verb verb() const;
        /**
         * @brief Resets the data connection, or ends it cleanly where the server may still be in its TLS handshake
         *        (DataConnection::serverMayBeInHandshake()): nothing was sent, so the server only gets an empty file,
         *        and its final reply gets the final reply timeout. A write or a finish() under way is always reset.
         *
         * @return Whether it was reset, which leaves the server a transfer to abort.
         */
        bool endData();

        TransferSession session_;
        UploadRequest request_;
        TransferOptions options_;
        std::shared_ptr<DataConnection> data_;
        std::shared_ptr<PendingCommand> store_;
        std::optional<Reply> earlyFinalReply_{};
        std::uint64_t bytes_{0u};
        /** @brief The data ended so that the server took all of it. */
        bool dataEnded_{false};
        bool aborted_{false};
        bool writing_{false};
        /** @brief finish() began to end the data. */
        bool ending_{false};
    };

    /**
     * @brief LIST, MLSD or NLST on its own data connection, on the strand of the control connection. The data goes
     *        through @p parser as it arrives; its end and a positive final reply, in either order, complete the
     *        listing.
     *
     * @param command The listing command, without an argument for the working directory.
     * @return The listing. Errors as for Download::start(); the data connection's error, after which the transfer is
     *         aborted with ABOR; the replyError() of a negative final reply; TimedOut when no final reply came, which
     *         a listing without a size cannot prove complete; Truncated for TLS data that ended without close_notify.
     */
    boost::asio::awaitable<std::expected<Listing, RemoteIo::RemoteError>>
    fetchListing(TransferSession session, Command command, ListingParser parser, TransferOptions options = {});
}
