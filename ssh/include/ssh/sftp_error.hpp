#pragma once

#include <remote_io/remote_error.hpp>

#include <fmt/format.h>

#include <string>

namespace SecureShell
{
    using WrapperErrors = RemoteIo::WrapperErrors;

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
                // Codes of SFTP versions 4 to 6 (draft-ietf-secsh-filexfer-13, 9.1), some servers send them anyway.
                case 9:
                    return "Invalid handle";
                case 10:
                    return "No such path";
                case 11:
                    return "File already exists";
                case 12:
                    return "Write protected";
                case 13:
                    return "No media";
                case 14:
                    return "No space on file system";
                case 15:
                    return "Quota exceeded";
                case 16:
                    return "Unknown principal";
                case 17:
                    return "Lock conflict";
                case 18:
                    return "Directory not empty";
                case 19:
                    return "Not a directory";
                case 20:
                    return "Invalid file name";
                case 21:
                    return "Link loop";
                case 22:
                    return "Cannot delete";
                case 23:
                    return "Invalid parameter";
                case 24:
                    return "File is a directory";
                case 25:
                    return "Byte range lock conflict";
                case 26:
                    return "Byte range lock refused";
                case 27:
                    return "Delete pending";
                case 28:
                    return "File corrupt";
                case 29:
                    return "Owner invalid";
                case 30:
                    return "Group invalid";
                case 31:
                    return "No matching byte range lock";
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
