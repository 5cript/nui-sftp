#pragma once

#include <ssh/file_stream_interface.hpp>
#include <remote_io/remote_error.hpp>
#include <ssh/file_information.hpp>

#include <libssh/sftp.h>

#include <memory>
#include <functional>
#include <future>
#include <expected>

namespace SecureShell
{
    class Session;
    class SftpSession;

    class FileStream
        : public IFileStream
        , public std::enable_shared_from_this<FileStream>
    {
      public:
        FileStream(std::shared_ptr<SftpSession> sftp, sftp_file file, sftp_limits_struct limits);
        ~FileStream() override;
        FileStream(FileStream const&) = delete;
        FileStream& operator=(FileStream const&) = delete;
        FileStream(FileStream&&) = delete;
        FileStream& operator=(FileStream&&) = delete;

        /**
         * @brief Moves the file pointer to the specified position.
         *
         * @param pos Where to move the file pointer to.
         */
        std::future<std::expected<void, RemoteIo::RemoteError>> seek(std::size_t pos) override;

        /**
         * @brief In-strand variant of seek. Must be called from within the processing thread.
         */
        std::expected<void, RemoteIo::RemoteError> seekInStrand(std::size_t pos) override;

        /**
         * @brief Tells the current position in the file.
         *
         * @return std::future<std::expected<std::size_t, RemoteIo::RemoteError>> The current position or an error.
         */
        std::future<std::expected<std::size_t, RemoteIo::RemoteError>> tell() override;

        /**
         * @brief In-strand variant of tell. Must be called from within the processing thread.
         */
        std::expected<std::size_t, RemoteIo::RemoteError> tellInStrand() override;

        /**
         * @brief Retrieves information about the file.
         *
         * @return std::future<std::expected<FileInformation, RemoteIo::RemoteError>>
         */
        std::future<std::expected<FileInformation, RemoteIo::RemoteError>> stat() override;

        /**
         * @brief In-strand variant of stat. Must be called from within the processing thread.
         */
        std::expected<FileInformation, RemoteIo::RemoteError> statInStrand() override;

        /**
         * @brief Rewind the file to the beginning.
         */
        std::future<std::expected<void, RemoteIo::RemoteError>> rewind() override;

        /**
         * @brief In-strand variant of rewind. Must be called from within the processing thread.
         */
        std::expected<void, RemoteIo::RemoteError> rewindInStrand() override;

        /**
         * @brief Reads some bytes from the file. Not necessarily fills the buffer. bufferSize MUST be less than or
         * equal to the read limit.
         *
         * @param buffer
         * @param bufferSize
         * @return std::future<std::expected<std::size_t, RemoteIo::RemoteError>>
         */
        std::future<std::expected<std::size_t, RemoteIo::RemoteError>>
        readSome(char* buffer, std::size_t bufferSize) override;

        /**
         * @brief In-strand variant of readSome. Must be called from within the processing thread.
         */
        std::expected<std::size_t, RemoteIo::RemoteError>
        readSomeInStrand(char* buffer, std::size_t bufferSize) override;

        /**
         * @brief Reads all bytes from the file.
         *
         * @param onRead Function called when data is read.
         * @return std::future<std::expected<std::size_t, RemoteIo::RemoteError>> The number of bytes read or an error.
         */
        std::future<std::expected<std::size_t, RemoteIo::RemoteError>>
        readAll(std::function<bool(std::string_view data)> onRead) override;

        /**
         * @brief Writes some bytes to the file.
         * Makes sure that all data is written even if the data is larger than the write limit by breaking it into
         * smaller parts.
         *
         * @param buffer
         * @param bufferSize
         * @return std::future<std::expected<void, RemoteIo::RemoteError>>
         */
        std::future<std::expected<void, RemoteIo::RemoteError>> write(std::string_view data) override;

        /**
         * @brief In-strand variant of write. Must be called from within the processing thread.
         */
        std::expected<void, RemoteIo::RemoteError> writeInStrand(std::string_view data) override;

        /**
         * @brief Returns the maximum number of bytes that can be written in a single pure write operation.
         * This limit is not necessary to uphold for the write function of this class.
         *
         * @return std::size_t
         */
        std::size_t writeLengthLimit() const override;

        /**
         * @brief Returns the maximum number of bytes that can be read in a single pure read operation.
         *
         * @return std::size_t
         */
        std::size_t readLengthLimit() const override;

        /**
         * @brief Returns an AsyncTransferContext that can be used to monitor the asynchronou transfer.
         *
         * @param buffer The buffer to read into. This function assumes EXCLUSIVE access.
         * @param bufferSize The size of the buffer.
         * @param onRead Called when data is read. Return false to stop reading.
         * @return std::shared_ptr<RemoteIo::AsyncTransferContext>
         */
        std::future<std::expected<std::shared_ptr<RemoteIo::AsyncTransferContext>, RemoteIo::RemoteError>> readAsync(
            SignedSizeType totalFileSize,
            char* buffer,
            SignedSizeType bufferSize,
            std::function<bool(SignedSizeType)> onRead
        ) override;

        /**
         * @brief In-strand variant of readAsync. Must be called from within the processing thread.
         */
        std::expected<std::shared_ptr<RemoteIo::AsyncTransferContext>, RemoteIo::RemoteError> readAsyncInStrand(
            SignedSizeType totalFileSize,
            char* buffer,
            SignedSizeType bufferSize,
            std::function<bool(SignedSizeType)> onRead
        ) override;

        std::future<std::expected<std::shared_ptr<RemoteIo::AsyncTransferContext>, RemoteIo::RemoteError>> writeAsync(
            SignedSizeType totalFileSize,
            char* buffer,
            SignedSizeType bufferSize,
            std::function<SignedSizeType(SignedSizeType)> doRead
        ) override;

        /**
         * @brief In-strand variant of writeAsync. Must be called from within the processing thread.
         */
        std::expected<std::shared_ptr<RemoteIo::AsyncTransferContext>, RemoteIo::RemoteError> writeAsyncInStrand(
            SignedSizeType totalFileSize,
            char* buffer,
            SignedSizeType bufferSize,
            std::function<SignedSizeType(SignedSizeType)> doRead
        ) override;

        /**
         * @brief Closes the file, the answer to the CLOSE carries errors the server only reports then, like those of a
         *        network filesystem. The stream reads and writes no more, closeInStrand still removes it.
         */
        std::expected<void, RemoteIo::RemoteError> finishInStrand() override;

        /**
         * @brief Brings this class into an invalid state and returns the sftp_file. The ownership of the file is
         * transferred to the caller.
         *
         * @return sftp_file
         */
        sftp_file release() override;

        void close() override;

        /**
         * @brief Closes the file and removes itself from the sftp session.
         * @param isBackElement Whether this is the last stream of the session, which is then removed faster.
         */
        void close(bool isBackElement);

        void closeInStrand() override;

        /**
         * @brief In-strand variant of close. Must be called from within the processing thread.
         */
        void closeInStrand(bool isBackElement);

        RemoteIo::ProcessingStrand* strand() const override;

      private:
        std::function<void(sftp_file)> makeFileDeleter();

        /**
         * @brief A transfer context that wakes this stream's processing thread when it resumes, fails or is canceled.
         */
        std::shared_ptr<RemoteIo::AsyncTransferContext> makeTransferContext();

        /**
         * @brief Lets closing the file first wait for the requests @p pipeline still has in flight, whose answers
         *        would otherwise stay queued in the sftp session.
         */
        template <typename PipelineT>
        void watchInFlight(std::shared_ptr<PipelineT> const& pipeline);

        template <typename FunctionT>
        void perform(FunctionT&& func);

        template <typename FunctionT>
        auto performPromise(FunctionT&& func);

        /**
         * @brief SftpSession::awaitServer of the session of this stream, the bare call when it is gone.
         */
        template <typename FunctionT, typename... ArgumentTs>
        auto awaitServer(FunctionT&& function, ArgumentTs&&... arguments);

        /**
         * @brief The open file whose answers in flight can still be waited for, null when it is closed or its channel
         *        was given up.
         */
        sftp_file answeringFile() const;

        RemoteIo::RemoteError lastError() const;

        void writePart(
            std::string_view toWrite,
            std::function<void(std::expected<void, RemoteIo::RemoteError>&&)> onWriteComplete
        );

      private:
        std::weak_ptr<SftpSession> sftp_;
        std::unique_ptr<sftp_file_struct, std::function<void(sftp_file)>> file_;
        sftp_limits_struct limits_;
        /** @brief Discards the requests of the running transfer, see watchInFlight. On the processing thread. */
        std::function<void(sftp_file)> discardInFlight_{};
    };
}