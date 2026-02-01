#pragma once

#include <fmt/format.h>

#include <string>

namespace SecureShell
{
    enum class WrapperErrors
    {
        None,
        OwnerNull,
        SharedPtrDestroyed,
        // This happens when the client does not respect the server max_write_length.
        // See: https://api.libssh.org/stable/structsftp__limits__struct.html
        ShortWrite,
        FileNull,
        TaskPushFailed,
    };

    struct SftpError
    {
        std::string message;
        // Could use a union or variant, but would require lots of code changes:
        int sshError = 0;
        int sftpError = 0;
        WrapperErrors wrapperError = WrapperErrors::None;

        inline std::string toString() const
        {
            return fmt::format(
                "SftpError: message: {}, sshError: {}, sftpError: {}, wrapperError: {}",
                message,
                sshError,
                sftpError,
                static_cast<int>(wrapperError)
            );
        }
    };
}