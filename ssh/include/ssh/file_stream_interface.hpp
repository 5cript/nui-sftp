#pragma once

#include <remote_io/async/processing_strand.hpp>
#include <remote_io/async/transfer_context.hpp>
#include <remote_io/remote_error.hpp>
#include <remote_io/remote_file_stream.hpp>
#include <ssh/file_information.hpp>

#include <libssh/sftp.h>

#include <functional>
#include <future>
#include <expected>

namespace SecureShell
{
    class Session;
    class SftpSession;

    /**
     * @brief An open SFTP file, which also has the deferred variants of the stream methods.
     */
    class IFileStream : public RemoteIo::IRemoteFileStream
    {
      public:
        IFileStream() = default;
        ~IFileStream() override = default;
        IFileStream(IFileStream const&) = delete;
        IFileStream& operator=(IFileStream const&) = delete;
        IFileStream(IFileStream&&) = delete;
        IFileStream& operator=(IFileStream&&) = delete;

        /**
         * @brief Moves the file pointer to the specified position.
         *
         * @param pos Where to move the file pointer to.
         */
        virtual std::future<std::expected<void, RemoteIo::RemoteError>> seek(std::size_t pos) = 0;

        /**
         * @brief Tells the current position in the file.
         *
         * @return std::future<std::expected<std::size_t, RemoteIo::RemoteError>> The current position or an error.
         */
        virtual std::future<std::expected<std::size_t, RemoteIo::RemoteError>> tell() = 0;

        /**
         * @brief In-strand variant of tell. Must be called from within the processing thread.
         */
        virtual std::expected<std::size_t, RemoteIo::RemoteError> tellInStrand() = 0;

        /**
         * @brief Retrieves information about the file.
         *
         * @return std::future<std::expected<FileInformation, RemoteIo::RemoteError>>
         */
        virtual std::future<std::expected<FileInformation, RemoteIo::RemoteError>> stat() = 0;

        /**
         * @brief Rewind the file to the beginning.
         */
        virtual std::future<std::expected<void, RemoteIo::RemoteError>> rewind() = 0;

        /**
         * @brief In-strand variant of rewind. Must be called from within the processing thread.
         */
        virtual std::expected<void, RemoteIo::RemoteError> rewindInStrand() = 0;

        /**
         * @brief Reads some bytes from the file. Not necessarily fills the buffer. bufferSize MUST be less than or
         * equal to the read limit.
         *
         * @param buffer
         * @param bufferSize
         * @return std::future<std::expected<std::size_t, RemoteIo::RemoteError>> The number of bytes read or an error.
         */
        virtual std::future<std::expected<std::size_t, RemoteIo::RemoteError>>
        readSome(char* buffer, std::size_t bufferSize) = 0;

        /**
         * @brief Returns an AsyncTransferContext that can be used to monitor the asynchronou transfer.
         *
         * @param buffer The buffer to read into. This function assumes EXCLUSIVE access.
         * @param bufferSize The size of the buffer.
         * @param onRead Called when data is read. Return false to stop reading.
         * @return std::shared_ptr<RemoteIo::AsyncTransferContext>
         */
        virtual std::future<std::expected<std::shared_ptr<RemoteIo::AsyncTransferContext>, RemoteIo::RemoteError>>
        readAsync(
            SignedSizeType totalFileSize,
            char* buffer,
            SignedSizeType bufferSize,
            std::function<bool(SignedSizeType)> onRead
        ) = 0;

        /**
         * @brief Returns an AsyncTransferContext that can be used to monitor the asynchronou transfer.
         *
         * @param buffer The buffer to read into. This function assumes EXCLUSIVE access.
         * @param bufferSize The size of the buffer.
         * @param doRead Called to read from the file into buffer. Should return the number of bytes read.
         * @return std::shared_ptr<RemoteIo::AsyncTransferContext>
         */
        virtual std::future<std::expected<std::shared_ptr<RemoteIo::AsyncTransferContext>, RemoteIo::RemoteError>>
        writeAsync(
            SignedSizeType totalFileSize,
            char* buffer,
            SignedSizeType bufferSize,
            std::function<SignedSizeType(SignedSizeType)> doRead
        ) = 0;

        /**
         * @brief Writes some bytes to the file.
         * Makes sure that all data is written even if the data is larger than the write limit by breaking it into
         * smaller parts.
         *
         * @param buffer
         * @param bufferSize
         * @return std::future<std::expected<void, RemoteIo::RemoteError>>
         */
        virtual std::future<std::expected<void, RemoteIo::RemoteError>> write(std::string_view data) = 0;

        /**
         * @brief Brings this class into an invalid state and returns the sftp_file. The ownership of the file is
         * transferred to the caller.
         *
         * @return sftp_file
         */
        virtual sftp_file release() = 0;
    };
}