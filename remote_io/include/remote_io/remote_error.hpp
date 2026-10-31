#pragma once

#include <utility/algorithm/case_convert.hpp>
#include <utility/convert_naming_convention.hpp>
#include <utility/describe.hpp>

#include <fmt/format.h>

#include <string>
#include <string_view>

namespace RemoteIo
{
    /**
     * @brief The protocol whose client raised an error.
     */
    enum class Protocol
    {
        None,
        Ssh,
        Sftp,
        Ftp,
    };
    BOOST_DESCRIBE_ENUM(Protocol, None, Ssh, Sftp, Ftp)

    /**
     * @brief What a remote error means, the same for every protocol.
     */
    enum class RemoteErrorKind
    {
        // The server or connection reported nothing, a wrapper error or the message tells what failed.
        None,
        NoSuchFile,
        PermissionDenied,
        AlreadyExists,
        NoSpace,
        QuotaExceeded,
        ConnectionLost,
        NoConnection,
        // The server refused another connection.
        ConnectionLimit,
        // No connection is free right now.
        Busy,
        TimedOut,
        Cancelled,
        Unsupported,
        EndOfFile,
        // The transfer ended with fewer bytes than the file has.
        Truncated,
        // The server's certificate is not trusted yet.
        TrustRequired,
        ProtocolViolation,
        AuthenticationFailed,
        Failure,
    };
    BOOST_DESCRIBE_ENUM(
        RemoteErrorKind,
        None,
        NoSuchFile,
        PermissionDenied,
        AlreadyExists,
        NoSpace,
        QuotaExceeded,
        ConnectionLost,
        NoConnection,
        ConnectionLimit,
        Busy,
        TimedOut,
        Cancelled,
        Unsupported,
        EndOfFile,
        Truncated,
        TrustRequired,
        ProtocolViolation,
        AuthenticationFailed,
        Failure
    )

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
        BufferUnavailable,
        // The remote file ended before the expected number of bytes was read.
        UnexpectedEndOfFile,
        // The local side of a transfer could not be read or written.
        LocalIoFailure,
        // The remote path is unset or invalid, nothing was sent to the server.
        InvalidPath,
    };
    BOOST_DESCRIBE_ENUM(
        WrapperErrors,
        None,
        OwnerNull,
        SharedPtrDestroyed,
        ShortWrite,
        FileNull,
        TaskPushFailed,
        BufferUnavailable,
        UnexpectedEndOfFile,
        LocalIoFailure,
        InvalidPath
    )

    namespace Detail
    {
        /**
         * @brief The name of an enumerator split into lower case words, "NoSuchFile" becomes "no such file".
         * @param value The enumerator.
         * @param unknown The text for a value that is not described.
         */
        template <typename EnumType>
        std::string enumeratorInWords(EnumType value, std::string_view unknown)
        {
            auto const* name = boost::describe::enum_to_string(value, nullptr);
            if (name == nullptr)
                return std::string{unknown};
            return Utility::splitByPascalCase(name).joined(" ");
        }
    }

    inline std::string toString(Protocol protocol)
    {
        if (protocol == Protocol::None)
            return "Remote";
        return Utility::Algorithm::toUpperCase(boost::describe::enum_to_string(protocol, "Unknown"));
    }

    inline std::string toString(RemoteErrorKind kind)
    {
        return Detail::enumeratorInWords(kind, "unknown kind");
    }

    inline std::string toString(WrapperErrors error)
    {
        return Detail::enumeratorInWords(error, "unknown wrapper error");
    }

    /**
     * @brief An error of a remote file system, whatever protocol it speaks.
     */
    struct RemoteError
    {
        RemoteErrorKind kind{RemoteErrorKind::None};
        Protocol protocol{Protocol::None};
        /** @brief The status or reply code of the protocol, 0 for none. */
        int protocolCode{0};
        std::string message{};
        WrapperErrors wrapperError{WrapperErrors::None};

        std::string toString() const
        {
            const auto kindText =
                kind == RemoteErrorKind::None ? std::string{} : fmt::format(" '{}'", RemoteIo::toString(kind));
            const auto codeText = protocolCode == 0 ? std::string{} : fmt::format(" (#{})", protocolCode);
            const auto messageText = message.empty() ? std::string{} : fmt::format(": {}", message);
            const auto wrapperText = wrapperError == WrapperErrors::None
                ? std::string{}
                : fmt::format(" (wrapper error: {})", RemoteIo::toString(wrapperError));
            return fmt::format(
                "{} error{}{}{}{}", RemoteIo::toString(protocol), kindText, codeText, messageText, wrapperText
            );
        }
    };
}
