#pragma once

#include <ssh/sftp_error_conversion.hpp>

#include <gtest/gtest.h>
#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <utility>
#include <vector>

namespace SecureShell::Test
{
    using RemoteIo::Protocol;
    using RemoteIo::RemoteErrorKind;

    TEST(SftpErrorConversionTests, SftpStatusesBecomeKinds)
    {
        const std::vector<std::pair<int, RemoteErrorKind>> expectations{
            {SSH_FX_OK, RemoteErrorKind::None},
            {SSH_FX_EOF, RemoteErrorKind::EndOfFile},
            {SSH_FX_NO_SUCH_FILE, RemoteErrorKind::NoSuchFile},
            {SSH_FX_PERMISSION_DENIED, RemoteErrorKind::PermissionDenied},
            {SSH_FX_FAILURE, RemoteErrorKind::Failure},
            {SSH_FX_BAD_MESSAGE, RemoteErrorKind::ProtocolViolation},
            {SSH_FX_NO_CONNECTION, RemoteErrorKind::NoConnection},
            {SSH_FX_CONNECTION_LOST, RemoteErrorKind::ConnectionLost},
            {SSH_FX_OP_UNSUPPORTED, RemoteErrorKind::Unsupported},
            {SSH_FX_INVALID_HANDLE, RemoteErrorKind::Failure},
            {SSH_FX_NO_SUCH_PATH, RemoteErrorKind::NoSuchFile},
            {SSH_FX_FILE_ALREADY_EXISTS, RemoteErrorKind::AlreadyExists},
            {SSH_FX_WRITE_PROTECT, RemoteErrorKind::PermissionDenied},
            {SSH_FX_NO_MEDIA, RemoteErrorKind::Failure},
            {14, RemoteErrorKind::NoSpace},
            {15, RemoteErrorKind::QuotaExceeded},
            {17, RemoteErrorKind::Failure},
            {18, RemoteErrorKind::Failure},
            {22, RemoteErrorKind::PermissionDenied},
            {31, RemoteErrorKind::Failure},
            {32, RemoteErrorKind::Failure},
            {-1, RemoteErrorKind::Failure},
        };

        for (auto const& [status, kind] : expectations)
        {
            const auto converted = toRemoteError(SftpError{.sftpError = status});
            EXPECT_EQ(converted.kind, kind) << "status " << status;
            EXPECT_EQ(converted.protocol, Protocol::Sftp) << "status " << status;
            EXPECT_EQ(converted.protocolCode, status) << "status " << status;
        }
    }

    TEST(SftpErrorConversionTests, MessageAndWrapperErrorCarryOver)
    {
        const auto converted = toRemoteError(
            SftpError{.message = "Buffer pool exhausted", .wrapperError = WrapperErrors::BufferUnavailable}
        );

        EXPECT_EQ(converted.kind, RemoteErrorKind::None);
        EXPECT_EQ(converted.protocol, Protocol::Sftp);
        EXPECT_EQ(converted.protocolCode, 0);
        EXPECT_EQ(converted.message, "Buffer pool exhausted");
        EXPECT_EQ(converted.wrapperError, WrapperErrors::BufferUnavailable);
    }

    TEST(SftpErrorConversionTests, StatusWithoutMessageIsNamedInTheMessage)
    {
        EXPECT_EQ(toRemoteError(SftpError{.sftpError = SSH_FX_NO_SUCH_FILE}).message, "No such file");
        EXPECT_EQ(toRemoteError(SftpError{.sftpError = 17}).message, "Lock conflict");
        EXPECT_EQ(toRemoteError(SftpError{.message = "gone", .sftpError = SSH_FX_NO_SUCH_FILE}).message, "gone");
        EXPECT_EQ(toRemoteError(SftpError{}).message, "");
    }

    TEST(SftpErrorConversionTests, SshErrorWithoutSftpStatusIsAnSshError)
    {
        const auto fatal = toRemoteError(SftpError{.message = "Socket error", .sshError = SSH_FATAL});
        EXPECT_EQ(fatal.kind, RemoteErrorKind::ConnectionLost);
        EXPECT_EQ(fatal.protocol, Protocol::Ssh);
        EXPECT_EQ(fatal.protocolCode, SSH_FATAL);
        EXPECT_EQ(fatal.message, "Socket error");

        const auto denied = toRemoteError(SftpError{.sshError = SSH_REQUEST_DENIED});
        EXPECT_EQ(denied.kind, RemoteErrorKind::PermissionDenied);
        EXPECT_EQ(denied.protocol, Protocol::Ssh);
    }

    TEST(SftpErrorConversionTests, SftpStatusWinsOverSshError)
    {
        const auto converted = toRemoteError(SftpError{.sshError = SSH_FATAL, .sftpError = SSH_FX_NO_SUCH_FILE});

        EXPECT_EQ(converted.kind, RemoteErrorKind::NoSuchFile);
        EXPECT_EQ(converted.protocol, Protocol::Sftp);
        EXPECT_EQ(converted.protocolCode, SSH_FX_NO_SUCH_FILE);
    }
}
