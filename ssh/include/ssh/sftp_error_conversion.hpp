#pragma once

#include <remote_io/remote_error.hpp>
#include <ssh/sftp_error.hpp>

namespace SecureShell
{
    /**
     * @brief The protocol neutral form of an SFTP error.
     *
     * The SFTP status becomes the protocol code, and its name the message when the error has none. Without a status, an
     * SSH error code makes it an SSH error.
     *
     * @param error The error of an SFTP session or one of its files.
     */
    RemoteIo::RemoteError toRemoteError(SftpError const& error);
}
