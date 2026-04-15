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
        MOCK_METHOD((std::future<std::expected<void, SftpError>>), seek, (std::size_t), (override));
        MOCK_METHOD((std::expected<void, SftpError>), seekInStrand, (std::size_t), (override));
        MOCK_METHOD((std::future<std::expected<std::size_t, SftpError>>), tell, (), (override));
        MOCK_METHOD((std::expected<std::size_t, SftpError>), tellInStrand, (), (override));
        MOCK_METHOD((std::future<std::expected<FileInformation, SftpError>>), stat, (), (override));
        MOCK_METHOD((std::expected<FileInformation, SftpError>), statInStrand, (), (override));
        MOCK_METHOD((std::future<std::expected<void, SftpError>>), rewind, (), (override));
        MOCK_METHOD((std::expected<void, SftpError>), rewindInStrand, (), (override));
        MOCK_METHOD(
            (std::future<std::expected<std::size_t, SftpError>>),
            readSome,
            (char* buffer, std::size_t bufferSize),
            (override)
        );
        MOCK_METHOD(
            (std::expected<std::size_t, SftpError>),
            readSomeInStrand,
            (char* buffer, std::size_t bufferSize),
            (override)
        );
        MOCK_METHOD(
            (std::future<std::expected<std::size_t, SftpError>>),
            readAll,
            (std::function<bool(std::string_view data)> onRead),
            (override)
        );
        using OnReadFuncT = std::function<bool(SecureShell::IFileStream::SignedSizeType)>;
        MOCK_METHOD(
            (std::future<std::expected<std::shared_ptr<AsyncTransferContext>, SftpError>>),
            readAsync,
            (SecureShell::IFileStream::SignedSizeType totalFileSize,
                char* buffer,
                SecureShell::IFileStream::SignedSizeType bufferSize,
                OnReadFuncT onRead),
            (override)
        );
        MOCK_METHOD(
            (std::expected<std::shared_ptr<AsyncTransferContext>, SftpError>),
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
            (std::future<std::expected<std::shared_ptr<AsyncTransferContext>, SftpError>>),
            writeAsync,
            (SecureShell::IFileStream::SignedSizeType totalFileSize,
                char* buffer,
                SecureShell::IFileStream::SignedSizeType bufferSize,
                DoReadFuncT doRead),
            (override)
        );
        MOCK_METHOD(
            (std::expected<std::shared_ptr<AsyncTransferContext>, SftpError>),
            writeAsyncInStrand,
            (SecureShell::IFileStream::SignedSizeType totalFileSize,
                char* buffer,
                SecureShell::IFileStream::SignedSizeType bufferSize,
                DoReadFuncT doRead),
            (override)
        );
        MOCK_METHOD((std::future<std::expected<void, SftpError>>), write, (std::string_view data), (override));
        MOCK_METHOD((std::expected<void, SftpError>), writeInStrand, (std::string_view data), (override));
        MOCK_METHOD(std::size_t, writeLengthLimit, (), (const, override));
        MOCK_METHOD(std::size_t, readLengthLimit, (), (const, override));
        MOCK_METHOD(sftp_file, release, (), (override));
        MOCK_METHOD(void, close, (bool isBackElement), (override));
        MOCK_METHOD(void, closeInStrand, (bool isBackElement), (override));
        MOCK_METHOD(ProcessingStrand*, strand, (), (const, override));
    };
}