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
        std::string message{};
        // Could use a union or variant, but would require lots of code changes:
        int sshError = 0;
        int sftpError = 0;
        WrapperErrors wrapperError = WrapperErrors::None;

        inline static std::string sftpErrorToComprehensible(int sftpError)
        {
            switch (sftpError)
            {
                case 0:
                    return "Ok";
                case 1:
                    return "End of file";
                case 2:
                    return "No such file";
                case 3:
                    return "Permission denied";
                case 4:
                    return "Failure";
                case 5:
                    return "Bad message";
                case 6:
                    return "No connection";
                case 7:
                    return "Connection lost";
                case 8:
                    return "Operation unsupported";
                default:
                    return fmt::format("Unknown error code: {}", sftpError);
            }
        }

        inline std::string toString() const
        {
            return fmt::format(
                "SFTP Error '{}' (#{}): {}. (Wrapper Error: {}).",
                sftpErrorToComprehensible(sftpError),
                sftpError,
                message,
                static_cast<int>(wrapperError)
            );
        }
    };
}