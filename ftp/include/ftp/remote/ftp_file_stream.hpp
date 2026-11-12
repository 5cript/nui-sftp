#pragma once

#include <ftp/client/transfer.hpp>
#include <ftp/engine/engine.hpp>
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
     * it ends a transfer that did not finish with ABOR, behind which the connection drains, and gives the connection
     * back.
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
         * @brief The attributes of the path, asked on a connection for commands: the stream's own runs the transfer.
         */
        std::expected<RemoteIo::FileInformation, RemoteIo::RemoteError> statInStrand() override;

        std::expected<std::size_t, RemoteIo::RemoteError> readSomeInStrand(char* buffer, std::size_t bufferSize) override;

        std::future<std::expected<std::size_t, RemoteIo::RemoteError>>
        readAll(std::function<bool(std::string_view data)> onRead) override;

        /**
         * @brief Writes @p data: STOR at the start, REST and STOR after a seek where the server has REST STREAM, else
         *        APPE, which continues at the end of the file.
         */
        std::expected<void, RemoteIo::RemoteError> writeInStrand(std::string_view data) override;

        std::expected<std::shared_ptr<RemoteIo::AsyncTransferContext>, RemoteIo::RemoteError> readAsyncInStrand(
            SignedSizeType totalFileSize,
            char* buffer,
            SignedSizeType bufferSize,
            std::function<bool(SignedSizeType)> onRead
        ) override;

        std::expected<std::shared_ptr<RemoteIo::AsyncTransferContext>, RemoteIo::RemoteError> writeAsyncInStrand(
            SignedSizeType totalFileSize,
            char* buffer,
            SignedSizeType bufferSize,
            std::function<SignedSizeType(SignedSizeType)> doRead
        ) override;

        /**
         * @brief The final reply of the transfer: a download read to its end, an upload with what was written, an
         *        upload that wrote nothing stores an empty file. Its result is kept, the stream moves nothing more.
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
            TransferSession session{};
            TransferOptions options{};
            std::shared_ptr<Download> download{};
            std::shared_ptr<Upload> upload{};
            /** @brief Where reads land, copied out on the processing thread once a call returned. */
            std::vector<char> readBuffer{};
        };

        using Error = RemoteIo::RemoteError;
        template <typename ValueT>
        using Result = std::expected<ValueT, Error>;

        static boost::asio::awaitable<Result<std::size_t>>
        readOn(std::shared_ptr<Transfer> transfer, DownloadRequest request, std::size_t limit);
        static boost::asio::awaitable<Result<void>> seekOn(std::shared_ptr<Transfer> transfer, std::uint64_t position);
        static boost::asio::awaitable<Result<void>>
        writeOn(std::shared_ptr<Transfer> transfer, UploadRequest request, std::string data);
        static boost::asio::awaitable<Result<void>> finishDownloadOn(std::shared_ptr<Transfer> transfer);
        static boost::asio::awaitable<Result<void>>
        finishUploadOn(std::shared_ptr<Transfer> transfer, UploadRequest request);

        /**
         * @brief Runs @p work on the connection's strand through Engine::call(), on the processing thread.
         */
        template <typename ValueT>
        Result<ValueT> callOnConnection(boost::asio::awaitable<Result<ValueT>> work)
        {
            return session_.engine->template call<ValueT>(
                [control = transfer_->session.control,
                    work = std::move(work)](std::shared_ptr<CallScope> const& scope) mutable
                {
                    return Detail::onConnection<ValueT>(control, scope, std::move(work));
                },
                options_.transferCallLimits
            );
        }

        /**
         * @brief Whether the stream may still move bytes @p direction.
         */
        Result<void> usable(Direction direction) const;
        /**
         * @brief Keeps @p error, the stream refuses more after a failed transfer.
         */
        Error broke(Error error);
        DownloadRequest downloadRequest() const;
        UploadRequest uploadRequest() const;
        Result<std::size_t> readAllInStrand(std::function<bool(std::string_view data)> const& onRead);
        /**
         * @brief Sends ABOR for a transfer that did not finish, then gives the connection back. Never waits.
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
        const std::shared_ptr<Transfer> transfer_;
        Lease lease_;
        /** @brief Where the transfer starts, set by a seek before the first read or write. */
        std::uint64_t offset_{0u};
        /** @brief A read or write asked for the transfer. */
        bool started_{false};
        std::optional<Result<void>> finished_{};
        std::optional<Error> broken_{};
        bool closed_{false};
    };
}
