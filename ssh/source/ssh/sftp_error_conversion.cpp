#include <ssh/sftp_error_conversion.hpp>

#include <libssh/libssh.h>
#include <libssh/sftp.h>

namespace SecureShell
{
    namespace
    {
        using RemoteIo::RemoteErrorKind;

        // Status codes of SFTP versions 4 to 6 (draft-ietf-secsh-filexfer-13, 9.1), libssh names only those up to 13.
        constexpr int noSpaceOnFilesystem = 14;
        constexpr int quotaExceeded = 15;
        constexpr int cannotDelete = 22;

        RemoteErrorKind kindOfSftpStatus(int status)
        {
            switch (status)
            {
                case SSH_FX_OK:
                    return RemoteErrorKind::None;
                case SSH_FX_EOF:
                    return RemoteErrorKind::EndOfFile;
                case SSH_FX_NO_SUCH_FILE:
                case SSH_FX_NO_SUCH_PATH:
                    return RemoteErrorKind::NoSuchFile;
                case SSH_FX_PERMISSION_DENIED:
                case SSH_FX_WRITE_PROTECT:
                case cannotDelete:
                    return RemoteErrorKind::PermissionDenied;
                case SSH_FX_BAD_MESSAGE:
                    return RemoteErrorKind::ProtocolViolation;
                case SSH_FX_NO_CONNECTION:
                    return RemoteErrorKind::NoConnection;
                case SSH_FX_CONNECTION_LOST:
                    return RemoteErrorKind::ConnectionLost;
                case SSH_FX_OP_UNSUPPORTED:
                    return RemoteErrorKind::Unsupported;
                case SSH_FX_FILE_ALREADY_EXISTS:
                    return RemoteErrorKind::AlreadyExists;
                case noSpaceOnFilesystem:
                    return RemoteErrorKind::NoSpace;
                case quotaExceeded:
                    return RemoteErrorKind::QuotaExceeded;
                default:
                    return RemoteErrorKind::Failure;
            }
        }

        RemoteErrorKind kindOfSshError(int code)
        {
            if (code == SSH_REQUEST_DENIED)
                return RemoteErrorKind::PermissionDenied;
            return RemoteErrorKind::ConnectionLost;
        }
    }

    RemoteIo::RemoteError toRemoteError(SftpError const& error)
    {
        if (error.sftpError == SSH_FX_OK && error.sshError != SSH_NO_ERROR)
        {
            return RemoteIo::RemoteError{
                .kind = kindOfSshError(error.sshError),
                .protocol = RemoteIo::Protocol::Ssh,
                .protocolCode = error.sshError,
                .message = error.message,
                .wrapperError = error.wrapperError,
            };
        }
        return RemoteIo::RemoteError{
            .kind = kindOfSftpStatus(error.sftpError),
            .protocol = RemoteIo::Protocol::Sftp,
            .protocolCode = error.sftpError,
            .message = error.message,
            .wrapperError = error.wrapperError,
        };
    }
}
