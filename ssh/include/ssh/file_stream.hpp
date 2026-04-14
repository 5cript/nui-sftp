#pragma once

#include <ssh/file_stream_interface.hpp>
#include <ssh/sftp_error.hpp>
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
        FileStream(FileStream&&);
        FileStream& operator=(FileStream&&);

        /**
         * @brief Moves the file pointer to the specified position.
         *
         * @param pos Where to move the file pointer to.
         */
        std::future<std::expected<void, SftpError>> seek(std::size_t pos) override;

        /**
         * @brief In-strand variant of seek. Must be called from within the processing thread.
         */
        std::expected<void, SftpError> seekInStrand(std::size_t pos) override;

        /**
         * @brief Tells the current position in the file.
         *
         * @return std::future<std::expected<std::size_t, SftpError>> The current position or an error.
         */
        std::future<std::expected<std::size_t, SftpError>> tell() override;

        /**
         * @brief In-strand variant of tell. Must be called from within the processing thread.
         */
        std::expected<std::size_t, SftpError> tellInStrand() override;

        /**
         * @brief Retrieves information about the file.
         *
         * @return std::future<std::expected<FileInformation, SftpError>>
         */
        std::future<std::expected<FileInformation, SftpError>> stat() override;

        /**
         * @brief In-strand variant of stat. Must be called from within the processing thread.
         */
        std::expected<FileInformation, SftpError> statInStrand() override;

        /**
         * @brief Rewind the file to the beginning.
         */
        std::future<std::expected<void, SftpError>> rewind() override;

        /**
         * @brief In-strand variant of rewind. Must be called from within the processing thread.
         */
        std::expected<void, SftpError> rewindInStrand() override;

        /**
         * @brief Reads some bytes from the file. Not necessarily fills the buffer. bufferSize MUST be less than or
         * equal to the read limit.
         *
         * @param buffer
         * @param bufferSize
         * @return std::future<std::expected<std::size_t, SftpError>>
         */
        std::future<std::expected<std::size_t, SftpError>> readSome(char* buffer, std::size_t bufferSize) override;

        /**
         * @brief In-strand variant of readSome. Must be called from within the processing thread.
         */
        std::expected<std::size_t, SftpError> readSomeInStrand(char* buffer, std::size_t bufferSize) override;

        /**
         * @brief Reads all bytes from the file.
         *
         * @param onRead Function called when data is read.
         * @return std::future<std::expected<std::size_t, SftpError>> The number of bytes read or an error.
         */
        std::future<std::expected<std::size_t, SftpError>>
        readAll(std::function<bool(std::string_view data)> onRead) override;

        /**
         * @brief Writes some bytes to the file.
         * Makes sure that all data is written even if the data is larger than the write limit by breaking it into
         * smaller parts.
         *
         * @param buffer
         * @param bufferSize
         * @return std::future<std::expected<void, SftpError>>
         */
        std::future<std::expected<void, SftpError>> write(std::string_view data) override;

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
         * @return std::shared_ptr<AsyncTransferContext>
         */
        std::future<std::expected<std::shared_ptr<AsyncTransferContext>, SftpError>> readAsync(
            SignedSizeType totalFileSize,
            char* buffer,
            SignedSizeType bufferSize,
            std::function<bool(SignedSizeType)> onRead
        ) override;

        std::future<std::expected<std::shared_ptr<AsyncTransferContext>, SftpError>> writeAsync(
            SignedSizeType totalFileSize,
            char* buffer,
            SignedSizeType bufferSize,
            std::function<SignedSizeType(SignedSizeType)> doRead
        ) override;

        /**
         * @brief Brings this class into an invalid state and returns the sftp_file. The ownership of the file is
         * transferred to the caller.
         *
         * @return sftp_file
         */
        sftp_file release() override;

        /**
         * @brief Closes the file and removes itself from the sftp session.
         */
        void close(bool isBackElement = false) override;

        /**
         * @brief In-strand variant of close. Must be called from within the processing thread.
         */
        void closeInStrand(bool isBackElement = false) override;

        ProcessingStrand* strand() const override;

      private:
        std::function<void(sftp_file)> makeFileDeleter();

        template <typename FunctionT>
        void perform(FunctionT&& func);

        template <typename FunctionT>
        auto performPromise(FunctionT&& func);

        SftpError lastError() const;

        void writePart(std::string_view toWrite, std::function<void(std::expected<void, SftpError>&&)> onWriteComplete);

      private:
        std::weak_ptr<SftpSession> sftp_;
        std::unique_ptr<sftp_file_struct, std::function<void(sftp_file)>> file_;
        sftp_limits_struct limits_;
    };
}