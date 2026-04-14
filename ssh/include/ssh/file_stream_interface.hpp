#pragma once

#include <ssh/async/processing_strand.hpp>
#include <ssh/sftp_error.hpp>
#include <ssh/file_information.hpp>
#include <ssh/async/transfer_context.hpp>

#include <libssh/sftp.h>

#include <functional>
#include <future>
#include <expected>

namespace SecureShell
{
    class Session;
    class SftpSession;

    class IFileStream
    {
      public:
        using SignedSizeType = std::make_signed_t<std::size_t>;

        IFileStream() = default;
        virtual ~IFileStream() = default;
        IFileStream(IFileStream const&) = default;
        IFileStream& operator=(IFileStream const&) = default;
        IFileStream(IFileStream&&) = default;
        IFileStream& operator=(IFileStream&&) = default;

        /**
         * @brief Moves the file pointer to the specified position.
         *
         * @param pos Where to move the file pointer to.
         */
        virtual std::future<std::expected<void, SftpError>> seek(std::size_t pos) = 0;

        /**
         * @brief In-strand variant of seek. Must be called from within the processing thread.
         */
        virtual std::expected<void, SftpError> seekInStrand(std::size_t pos) = 0;

        /**
         * @brief Tells the current position in the file.
         *
         * @return std::future<std::expected<std::size_t, SftpError>> The current position or an error.
         */
        virtual std::future<std::expected<std::size_t, SftpError>> tell() = 0;

        /**
         * @brief In-strand variant of tell. Must be called from within the processing thread.
         */
        virtual std::expected<std::size_t, SftpError> tellInStrand() = 0;

        /**
         * @brief Retrieves information about the file.
         *
         * @return std::future<std::expected<FileInformation, SftpError>>
         */
        virtual std::future<std::expected<FileInformation, SftpError>> stat() = 0;

        /**
         * @brief In-strand variant of stat. Must be called from within the processing thread.
         */
        virtual std::expected<FileInformation, SftpError> statInStrand() = 0;

        /**
         * @brief Rewind the file to the beginning.
         */
        virtual std::future<std::expected<void, SftpError>> rewind() = 0;

        /**
         * @brief In-strand variant of rewind. Must be called from within the processing thread.
         */
        virtual std::expected<void, SftpError> rewindInStrand() = 0;

        /**
         * @brief Reads some bytes from the file. Not necessarily fills the buffer. bufferSize MUST be less than or
         * equal to the read limit.
         *
         * @param buffer
         * @param bufferSize
         * @return std::future<std::expected<std::size_t, SftpError>> The number of bytes read or an error.
         */
        virtual std::future<std::expected<std::size_t, SftpError>> readSome(char* buffer, std::size_t bufferSize) = 0;

        /**
         * @brief In-strand variant of readSome. Must be called from within the processing thread.
         */
        virtual std::expected<std::size_t, SftpError> readSomeInStrand(char* buffer, std::size_t bufferSize) = 0;

        /**
         * @brief Reads all bytes from the file.
         *
         * @param onRead Let this function return false to stop reading.
         * @return std::future<std::expected<std::size_t, SftpError>> The number of bytes read or an error.
         */
        virtual std::future<std::expected<std::size_t, SftpError>>
        readAll(std::function<bool(std::string_view data)> onRead) = 0;

        /**
         * @brief Returns an AsyncTransferContext that can be used to monitor the asynchronou transfer.
         *
         * @param buffer The buffer to read into. This function assumes EXCLUSIVE access.
         * @param bufferSize The size of the buffer.
         * @param onRead Called when data is read. Return false to stop reading.
         * @return std::shared_ptr<AsyncTransferContext>
         */
        virtual std::future<std::expected<std::shared_ptr<AsyncTransferContext>, SftpError>> readAsync(
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
         * @return std::shared_ptr<AsyncTransferContext>
         */
        virtual std::future<std::expected<std::shared_ptr<AsyncTransferContext>, SftpError>> writeAsync(
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
         * @return std::future<std::expected<void, SftpError>>
         */
        virtual std::future<std::expected<void, SftpError>> write(std::string_view data) = 0;

        /**
         * @brief Returns the maximum number of bytes that can be written in a single pure write operation.
         * This limit is not necessary to uphold for the write function of this class.
         *
         * @return std::size_t
         */
        virtual std::size_t writeLengthLimit() const = 0;

        /**
         * @brief Returns the maximum number of bytes that can be read in a single pure read operation.
         *
         * @return std::size_t
         */
        virtual std::size_t readLengthLimit() const = 0;

        /**
         * @brief Brings this class into an invalid state and returns the sftp_file. The ownership of the file is
         * transferred to the caller.
         *
         * @return sftp_file
         */
        virtual sftp_file release() = 0;

        /**
         * @brief Closes the file and removes itself from the sftp session.
         */
        virtual void close(bool isBackElement = false) = 0;

        /**
         * @brief In-strand variant of close. Must be called from within the processing thread.
         */
        virtual void closeInStrand(bool isBackElement = false) = 0;

        /**
         * @brief Returns the processing strand of the file stream.
         *
         * @return void*
         */
        virtual ProcessingStrand* strand() const = 0;
    };
}