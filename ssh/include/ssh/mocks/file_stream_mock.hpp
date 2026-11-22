#pragma once

#include <ssh/file_stream_interface.hpp>

#include <gmock/gmock.h>

#include <future>
#include <optional>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <memory>

namespace SecureShell::Test
{
    class FileStreamMock : public SecureShell::IFileStream
    {
      public:
        MOCK_METHOD((std::future<std::expected<void, RemoteIo::RemoteError>>), seek, (std::size_t), (override));
        MOCK_METHOD((std::expected<void, RemoteIo::RemoteError>), seekInStrand, (std::size_t), (override));
        MOCK_METHOD((std::future<std::expected<std::size_t, RemoteIo::RemoteError>>), tell, (), (override));
        MOCK_METHOD((std::expected<std::size_t, RemoteIo::RemoteError>), tellInStrand, (), (override));
        MOCK_METHOD((std::future<std::expected<FileInformation, RemoteIo::RemoteError>>), stat, (), (override));
        MOCK_METHOD((std::expected<FileInformation, RemoteIo::RemoteError>), statInStrand, (), (override));
        MOCK_METHOD((std::future<std::expected<void, RemoteIo::RemoteError>>), rewind, (), (override));
        MOCK_METHOD((std::expected<void, RemoteIo::RemoteError>), rewindInStrand, (), (override));
        MOCK_METHOD(
            (std::future<std::expected<std::size_t, RemoteIo::RemoteError>>),
            readSome,
            (char* buffer, std::size_t bufferSize),
            (override)
        );
        MOCK_METHOD(
            (std::expected<std::size_t, RemoteIo::RemoteError>),
            readSomeInStrand,
            (char* buffer, std::size_t bufferSize),
            (override)
        );
        MOCK_METHOD(
            (std::future<std::expected<std::size_t, RemoteIo::RemoteError>>),
            readAll,
            (std::function<bool(std::string_view data)> onRead),
            (override)
        );
        using OnReadFuncT = std::function<bool(SecureShell::IFileStream::SignedSizeType)>;
        MOCK_METHOD(
            (std::future<std::expected<std::shared_ptr<RemoteIo::AsyncTransferContext>, RemoteIo::RemoteError>>),
            readAsync,
            (SecureShell::IFileStream::SignedSizeType totalFileSize,
                char* buffer,
                SecureShell::IFileStream::SignedSizeType bufferSize,
                OnReadFuncT onRead),
            (override)
        );
        MOCK_METHOD(
            (std::expected<std::shared_ptr<RemoteIo::AsyncTransferContext>, RemoteIo::RemoteError>),
            readAsyncInStrand,
            (SecureShell::IFileStream::SignedSizeType totalFileSize,
                char* buffer,
                SecureShell::IFileStream::SignedSizeType bufferSize,
                OnReadFuncT onRead),
            (override)
        );
        using DoReadFuncT =
            std::function<SecureShell::IFileStream::SignedSizeType(SecureShell::IFileStream::SignedSizeType)>;
        MOCK_METHOD(
            (std::future<std::expected<std::shared_ptr<RemoteIo::AsyncTransferContext>, RemoteIo::RemoteError>>),
            writeAsync,
            (SecureShell::IFileStream::SignedSizeType totalFileSize,
                char* buffer,
                SecureShell::IFileStream::SignedSizeType bufferSize,
                DoReadFuncT doRead),
            (override)
        );
        MOCK_METHOD(
            (std::expected<std::shared_ptr<RemoteIo::AsyncTransferContext>, RemoteIo::RemoteError>),
            writeAsyncInStrand,
            (SecureShell::IFileStream::SignedSizeType totalFileSize,
                char* buffer,
                SecureShell::IFileStream::SignedSizeType bufferSize,
                DoReadFuncT doRead),
            (override)
        );
        MOCK_METHOD(
            (std::future<std::expected<void, RemoteIo::RemoteError>>),
            write,
            (std::string_view data),
            (override)
        );
        MOCK_METHOD((std::expected<void, RemoteIo::RemoteError>), writeInStrand, (std::string_view data), (override));
        MOCK_METHOD(std::size_t, writeLengthLimit, (), (const, override));
        MOCK_METHOD(std::size_t, readLengthLimit, (), (const, override));
        MOCK_METHOD((std::expected<void, RemoteIo::RemoteError>), finishInStrand, (), (override));
        MOCK_METHOD(sftp_file, release, (), (override));
        MOCK_METHOD(void, close, (), (override));
        MOCK_METHOD(void, closeInStrand, (), (override));
        MOCK_METHOD(RemoteIo::ProcessingStrand*, strand, (), (const, override));
    };
}