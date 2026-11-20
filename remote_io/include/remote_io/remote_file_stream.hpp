#pragma once

#include <remote_io/async/processing_strand.hpp>
#include <remote_io/async/transfer_context.hpp>
#include <remote_io/remote_error.hpp>
#include <shared_data/directory_entry.hpp>

#include <cstddef>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <string_view>
#include <type_traits>

namespace RemoteIo
{
    using FileInformation = SharedData::DirectoryEntry;

    /**
     * @brief An open file of a remote file system. Its InStrand methods must run on the processing thread of its
     *        strand.
     */
    class IRemoteFileStream
    {
      public:
        using SignedSizeType = std::make_signed_t<std::size_t>;

        IRemoteFileStream() = default;
        virtual ~IRemoteFileStream() = default;
        IRemoteFileStream(IRemoteFileStream const&) = delete;
        IRemoteFileStream& operator=(IRemoteFileStream const&) = delete;
        IRemoteFileStream(IRemoteFileStream&&) = delete;
        IRemoteFileStream& operator=(IRemoteFileStream&&) = delete;

        /**
         * @brief Moves the position the next read or write starts at.
         * @param position The offset from the start of the file.
         */
        virtual std::expected<void, RemoteError> seekInStrand(std::size_t position) = 0;

        /**
         * @brief The attributes of the open file.
         */
        virtual std::expected<FileInformation, RemoteError> statInStrand() = 0;

        /**
         * @brief Reads some bytes, not necessarily as many as fit.
         * @param buffer Receives the bytes.
         * @param bufferSize At most @ref readLengthLimit.
         * @return The number of bytes read, 0 at the end of the file.
         */
        virtual std::expected<std::size_t, RemoteError> readSomeInStrand(char* buffer, std::size_t bufferSize) = 0;

        /**
         * @brief Reads the whole file on the strand of the stream.
         * @param onRead Gets each piece read, returns false to stop reading.
         * @return The number of bytes read.
         */
        virtual std::future<std::expected<std::size_t, RemoteError>>
        readAll(std::function<bool(std::string_view data)> onRead) = 0;

        /**
         * @brief Writes all of @p data, in pieces of at most @ref writeLengthLimit.
         */
        virtual std::expected<void, RemoteError> writeInStrand(std::string_view data) = 0;

        /**
         * @brief Starts reading the file in turns of the processing thread, the returned context reports on it.
         * @param totalFileSize How many bytes the file has, reading fewer fails the transfer as truncated.
         * @param buffer Receives the bytes, used only by the transfer until it ended.
         * @param bufferSize The size of @p buffer.
         * @param onRead Gets how many bytes @p buffer holds now, returns false to fail the transfer. Must not throw:
         *        an exception ends the session.
         */
        virtual std::expected<std::shared_ptr<AsyncTransferContext>, RemoteError> readAsyncInStrand(
            SignedSizeType totalFileSize,
            char* buffer,
            SignedSizeType bufferSize,
            std::function<bool(SignedSizeType)> onRead
        ) = 0;

        /**
         * @brief Starts writing the file in turns of the processing thread, the returned context reports on it.
         * @param totalFileSize How many bytes to write.
         * @param buffer Holds the bytes to write, used only by the transfer until it ended.
         * @param bufferSize The size of @p buffer.
         * @param doRead Puts at most the given number of bytes into @p buffer and returns how many it put there.
         *        Must not throw, as onRead of readAsyncInStrand().
         */
        virtual std::expected<std::shared_ptr<AsyncTransferContext>, RemoteError> writeAsyncInStrand(
            SignedSizeType totalFileSize,
            char* buffer,
            SignedSizeType bufferSize,
            std::function<SignedSizeType(SignedSizeType)> doRead
        ) = 0;

        /**
         * @brief Waits until the server confirmed the transfer of this file. Called after the last read or write, an
         *        error means the transfer did not succeed even though every byte moved.
         */
        virtual std::expected<void, RemoteError> finishInStrand() = 0;

        /**
         * @brief The most bytes one read asks the server for.
         */
        virtual std::size_t readLengthLimit() const = 0;

        /**
         * @brief The most bytes one write sends to the server.
         */
        virtual std::size_t writeLengthLimit() const = 0;

        /**
         * @brief Closes the file on the strand of the stream.
         */
        virtual void close() = 0;

        /**
         * @brief Closes the file.
         */
        virtual void closeInStrand() = 0;

        virtual ProcessingStrand* strand() const = 0;
    };
}
