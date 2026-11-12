#pragma once

#include <ftp/client/transfer.hpp>
#include <ftp/engine/delivery_ring.hpp>
#include <ftp/engine/engine.hpp>
#include <ftp/engine/ring.hpp>
#include <ftp/engine/supply_ring.hpp>
#include <ftp/pool/lease.hpp>
#include <ftp/remote/file_system_session.hpp>
#include <paths/remote_path.hpp>
#include <remote_io/async/processing_strand.hpp>
#include <remote_io/async/transfer_context.hpp>
#include <remote_io/remote_error.hpp>
#include <remote_io/remote_file_stream.hpp>

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FileTransferProtocol
{
    class FtpFileStream;

    /**
     * @brief The open streams of a file system, which its close ends. Thread-safe.
     */
    class OpenStreams
    {
      public:
        /**
         * @return False once closed, the stream is not kept then.
         */
        bool add(std::shared_ptr<FtpFileStream> stream);

        void remove(FtpFileStream const* stream);

        /**
         * @brief Refuses later streams.
         *
         * @return The streams kept, for the caller to close.
         */
        std::vector<std::shared_ptr<FtpFileStream>> close();

        std::size_t size() const;

      private:
        mutable std::mutex mutex_{};
        std::vector<std::shared_ptr<FtpFileStream>> streams_{};
        bool closed_{false};
    };

    /**
     * @brief One file of an FTP server opened for a transfer, on a connection of its own for as long as it is open.
     *
     * A download starts its RETR at the first read, with REST for a seek before it, an upload its STOR or APPE at the
     * first write. Each synchronous call runs on the connection's strand through Engine::call() and blocks the
     * processing thread meanwhile, the bytes go through a buffer of the stream, so a call that timed out never writes
     * into the caller's. Once a transfer failed the stream refuses more. closeInStrand() never waits for the server:
     * it ends a transfer that did not finish with ABOR, behind which the connection drains. The connection goes back
     * to the pool once nothing of the stream runs on it any more, a transfer that was still starting included.
     *
     * An asynchronous read or write carries the data through a ring between the connection's strand and the
     * processing thread, see DeliveryRing and SupplyRing, and continues a transfer the synchronous calls started.
     * Its context completes on the processing thread after the last delivery and the final reply. A pause stops the
     * ring, a cancel or a failure of the context stops it and sends ABOR.
     */
    class FtpFileStream
        : public RemoteIo::IRemoteFileStream
        , public std::enable_shared_from_this<FtpFileStream>
    {
        struct Private
        {};

      public:
        enum class Direction : std::uint8_t
        {
            Download,
            Upload,
        };

        /**
         * @param strand The strand of the file system, on the ProcessingThread of the session.
         * @param lease The connection the transfer runs on.
         * @param size The size of a file to download, to tell a cut transfer from a complete one.
         */
        static std::shared_ptr<FtpFileStream> create(
            FileSystemSession session,
            FtpFileSystemOptions options,
            RemoteIo::ProcessingStrand* strand,
            Lease lease,
            Paths::RemotePath path,
            Direction direction,
            std::optional<std::uint64_t> size,
            std::weak_ptr<OpenStreams> streams
        );

        FtpFileStream(
            Private,
            FileSystemSession session,
            FtpFileSystemOptions options,
            RemoteIo::ProcessingStrand* strand,
            Lease lease,
            Paths::RemotePath path,
            Direction direction,
            std::optional<std::uint64_t> size,
            std::weak_ptr<OpenStreams> streams
        );
        /**
         * @brief Ends a transfer that did not finish, as closeInStrand() does.
         */
        ~FtpFileStream() override;
        FtpFileStream(FtpFileStream const&) = delete;
        FtpFileStream& operator=(FtpFileStream const&) = delete;
        FtpFileStream(FtpFileStream&&) = delete;
        FtpFileStream& operator=(FtpFileStream&&) = delete;

        /**
         * @brief Before the first read or write, where the transfer starts. A download that runs moves as
         *        Download::seek() does: the position it reached costs nothing. An upload that runs cannot move.
         */
        std::expected<void, RemoteIo::RemoteError> seekInStrand(std::size_t position) override;

        /**
         * @brief The attributes of the path. Asked on the stream's own connection while no transfer runs there, before
         *        the first read or write and after a transfer that finished: it may be the only one of the session,
         *        which a connection for commands would wait for. Else on a connection for commands.
         */
        std::expected<RemoteIo::FileInformation, RemoteIo::RemoteError> statInStrand() override;

        std::expected<std::size_t, RemoteIo::RemoteError> readSomeInStrand(char* buffer, std::size_t bufferSize) override;

        std::future<std::expected<std::size_t, RemoteIo::RemoteError>>
        readAll(std::function<bool(std::string_view data)> onRead) override;

        /**
         * @brief Writes @p data: STOR at the start, REST and STOR after a seek where the server has REST STREAM, else
         *        APPE, which continues at the end of the file: Unsupported when SIZE shows that the file ends
         *        elsewhere than the seek. A server without SIZE gets the APPE unchecked.
         */
        std::expected<void, RemoteIo::RemoteError> writeInStrand(std::string_view data) override;

        /**
         * @brief Reads the rest of the file asynchronously, @p buffer gets each slot of the ring in pieces of
         *        @p bufferSize. Fewer bytes than @p totalFileSize fail the transfer as Truncated, also where the
         *        download took them because the file changed meanwhile.
         */
        std::expected<std::shared_ptr<RemoteIo::AsyncTransferContext>, RemoteIo::RemoteError> readAsyncInStrand(
            SignedSizeType totalFileSize,
            char* buffer,
            SignedSizeType bufferSize,
            std::function<bool(SignedSizeType)> onRead
        ) override;

        /**
         * @brief Writes @p totalFileSize bytes asynchronously, @p doRead fills the slots of the ring through
         *        @p buffer. A @p doRead that gives nothing before the end fails the transfer.
         */
        std::expected<std::shared_ptr<RemoteIo::AsyncTransferContext>, RemoteIo::RemoteError> writeAsyncInStrand(
            SignedSizeType totalFileSize,
            char* buffer,
            SignedSizeType bufferSize,
            std::function<SignedSizeType(SignedSizeType)> doRead
        ) override;

        /**
         * @brief The final reply of the transfer: a download read to its end, an upload with what was written, an
         *        upload that wrote nothing stores an empty file. Its result is kept, the stream moves nothing more.
         *        Refused while an asynchronous transfer runs, which finishes itself: its outcome is the result then.
         */
        std::expected<void, RemoteIo::RemoteError> finishInStrand() override;

        std::size_t readLengthLimit() const override;
        std::size_t writeLengthLimit() const override;

        void close() override;
        void closeInStrand() override;

        RemoteIo::ProcessingStrand* strand() const override;

      private:
        /**
         * @brief The transfer, used on the strand of the stream's control connection.
         */
        struct Transfer
        {
            /**
             * @brief The connection, given back when the transfer is gone: once the stream and every coroutine and
             *        handler of it on the connection let go of it.
             */
            Lease lease{};
            TransferSession session{};
            TransferOptions options{};
            std::shared_ptr<ServerDialect> dialect{};
            ListingLimits listing{};
            std::shared_ptr<Download> download{};
            std::shared_ptr<Upload> upload{};
            /** @brief Where reads land, copied out on the processing thread once a call returned. */
            std::vector<char> readBuffer{};

            /**
             * @brief What a metadata operation on the connection of the transfer runs on.
             */
            MetadataSession metadata() const;
        };

        /**
         * @brief How the upload of the stream starts.
         */
        struct UploadStart
        {
            UploadRequest request{};
            /** @brief Where the file has to end, for an APPE that stands in for a seek there. */
            std::optional<std::uint64_t> appendsAt{};
        };

        using Error = RemoteIo::RemoteError;
        template <typename ValueT>
        using Result = std::expected<ValueT, Error>;
        using Context = RemoteIo::AsyncTransferContext;

        /**
         * @brief What the wake of an asynchronous transfer's context reaches. Set before the context is handed out.
         */
        struct AsyncLinks
        {
            std::weak_ptr<Ring> ring{};
            std::weak_ptr<Context> context{};
            std::weak_ptr<Transfer> transfer{};
            std::weak_ptr<Engine> engine{};

            /**
             * @brief A resume wakes the ring, the end of the context stops it and sends ABOR. Any thread.
             */
            void wake() const;
        };

        std::shared_ptr<Context> makeContext(std::shared_ptr<Ring> const& ring) const;
        /**
         * @brief Hands the bytes of one slot to @p onRead in pieces of @p bufferSize, on the processing thread.
         */
        static bool deliver(
            Context& context,
            char* buffer,
            std::size_t bufferSize,
            std::function<bool(SignedSizeType)> const& onRead,
            std::span<char const> bytes
        );
        /**
         * @brief Fills one slot with @p doRead through @p buffer, on the processing thread.
         */
        static Result<std::size_t> fill(
            Context& context,
            char* buffer,
            SignedSizeType bufferSize,
            std::function<SignedSizeType(SignedSizeType)> const& doRead,
            SignedSizeType& remaining,
            std::span<char> slot
        );
        static boost::asio::awaitable<Result<void>> runDownloadRing(
            std::shared_ptr<Transfer> transfer,
            std::shared_ptr<DeliveryRing> ring,
            std::shared_ptr<Context> context,
            DownloadRequest request,
            DeliveryRing::Sink sink
        );
        static boost::asio::awaitable<Result<void>> runUploadRing(
            std::shared_ptr<Transfer> transfer,
            std::shared_ptr<SupplyRing> ring,
            std::shared_ptr<Context> context,
            UploadStart start,
            SupplyRing::Source source
        );
        /**
         * @brief Runs @p ringSide on the connection's strand, then ends the context with its outcome in a task on the
         *        processing thread, behind the last delivery. On the session strand.
         */
        static boost::asio::awaitable<void> runAsync(
            std::weak_ptr<FtpFileStream> stream,
            std::shared_ptr<Engine> engine,
            std::shared_ptr<ControlConnection> control,
            std::shared_ptr<Context> context,
            boost::asio::awaitable<Result<void>> ringSide,
            std::optional<SignedSizeType> expectedBytes
        );
        /**
         * @brief Keeps the outcome of the asynchronous transfer, on the processing thread.
         */
        void asyncEnded(Result<void> outcome);
        static void abortTransfer(Transfer& transfer);

        static boost::asio::awaitable<Result<std::size_t>>
        readOn(std::shared_ptr<Transfer> transfer, DownloadRequest request, std::size_t limit);
        static boost::asio::awaitable<Result<void>> seekOn(std::shared_ptr<Transfer> transfer, std::uint64_t position);
        /**
         * @brief Starts the upload of @p transfer unless it runs. Before an APPE that stands in for a seek, SIZE has
         *        to show the file ending there.
         */
        static boost::asio::awaitable<Result<void>> startUploadOn(std::shared_ptr<Transfer> transfer, UploadStart start);
        static boost::asio::awaitable<Result<void>>
        writeOn(std::shared_ptr<Transfer> transfer, UploadStart start, std::string data);
        static boost::asio::awaitable<Result<void>> finishDownloadOn(std::shared_ptr<Transfer> transfer);
        static boost::asio::awaitable<Result<void>> finishUploadOn(std::shared_ptr<Transfer> transfer, UploadStart start);

        /**
         * @brief The attributes of @p path asked on the connection of @p transfer, nothing when that connection is not
         *        open: a transfer whose final reply never came left it out of step.
         */
        static boost::asio::awaitable<Result<std::optional<RemoteIo::FileInformation>>>
        statOn(std::shared_ptr<Transfer> transfer, Paths::RemotePath path);

        /**
         * @brief Runs @p work on the connection's strand through Engine::call() within @p limits, on the processing
         *        thread.
         */
        template <typename ValueT>
        Result<ValueT> callOnConnection(boost::asio::awaitable<Result<ValueT>> work, SyncLimits limits)
        {
            return session_.engine->template call<ValueT>(
                [control = transfer_->session.control,
                    work = std::move(work)](std::shared_ptr<CallScope> const& scope) mutable
                {
                    return Detail::onConnection<ValueT>(control, scope, std::move(work));
                },
                limits
            );
        }

        /**
         * @brief No transfer of the stream runs on its connection: none started, or it finished with its final reply.
         */
        bool connectionIsFree() const;
        /**
         * @brief Whether the stream may still move bytes @p direction.
         */
        Result<void> usable(Direction direction) const;
        /**
         * @brief Keeps @p error, the stream refuses more after a failed transfer.
         */
        Error broke(Error error);
        DownloadRequest downloadRequest() const;
        UploadStart uploadStart() const;
        Result<std::size_t> readAllInStrand(std::function<bool(std::string_view data)> const& onRead);
        static std::shared_ptr<Transfer>
        makeTransfer(Lease lease, FileSystemSession const& session, FtpFileSystemOptions const& options);
        /**
         * @brief Sends ABOR for a transfer that did not finish and lets go of the transfer, which gives the connection
         *        back behind the ABOR and what still runs on it. Never waits.
         */
        void endTransfer();
        static Error closedError();

        const FileSystemSession session_;
        const FtpFileSystemOptions options_;
        RemoteIo::ProcessingStrand* const strand_;
        const Paths::RemotePath path_;
        const Direction direction_;
        const std::optional<std::uint64_t> size_;
        const std::weak_ptr<OpenStreams> streams_;
        /** @brief Null once the stream closed. */
        std::shared_ptr<Transfer> transfer_;
        /** @brief Where the transfer starts, set by a seek before the first read or write. */
        std::uint64_t offset_{0u};
        /** @brief A read or write asked for the transfer. */
        bool started_{false};
        std::optional<Result<void>> finished_{};
        std::optional<Error> broken_{};
        bool closed_{false};
        /** @brief The ring of the asynchronous transfer that runs. */
        std::shared_ptr<Ring> ring_{};
        std::shared_ptr<Context> context_{};
    };
}
