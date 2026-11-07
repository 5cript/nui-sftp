#pragma once

#include <remote_io/remote_error.hpp>
#include <utility/describe.hpp>

#include <expected>
#include <string>
#include <string_view>

namespace FileTransferProtocol
{
    /**
     * @brief Every command the client sends. Commands that are not here cannot be sent, among them STRU, MODE, CCC,
     *        PORT and EPRT.
     */
    enum class Verb
    {
        User,
        Password,
        Account,
        Authenticate,
        ProtectionBufferSize,
        DataProtection,
        Features,
        Options,
        RepresentationType,
        PrintWorkingDirectory,
        ChangeWorkingDirectory,
        ChangeToParentDirectory,
        MakeDirectory,
        RemoveDirectory,
        Delete,
        RenameFrom,
        RenameTo,
        Size,
        ModificationTime,
        SetModificationTime,
        Site,
        MachineListSingle,
        MachineListDirectory,
        Status,
        ExtendedPassive,
        Passive,
        Restart,
        Retrieve,
        Store,
        Append,
        List,
        NameList,
        Abort,
        Noop,
        Quit,
    };
    BOOST_DESCRIBE_ENUM(
        Verb,
        User,
        Password,
        Account,
        Authenticate,
        ProtectionBufferSize,
        DataProtection,
        Features,
        Options,
        RepresentationType,
        PrintWorkingDirectory,
        ChangeWorkingDirectory,
        ChangeToParentDirectory,
        MakeDirectory,
        RemoveDirectory,
        Delete,
        RenameFrom,
        RenameTo,
        Size,
        ModificationTime,
        SetModificationTime,
        Site,
        MachineListSingle,
        MachineListDirectory,
        Status,
        ExtendedPassive,
        Passive,
        Restart,
        Retrieve,
        Store,
        Append,
        List,
        NameList,
        Abort,
        Noop,
        Quit
    )

    /**
     * @brief The three or four letters of @p verb on the wire, "RETR" for Verb::Retrieve.
     */
    std::string_view wireName(Verb verb);

    struct Command
    {
        Verb verb{Verb::Noop};
        /** @brief The bytes after the space, empty for a command without one. */
        std::string argument{};

        /**
         * @brief The bytes to send: the verb, a space and the argument when there is one, CRLF (RFC 959 5.3). 0xFF in
         *        the argument is doubled, Telnet's IAC IAC.
         *
         * @return InvalidPath when the argument holds a CR, an LF or a NUL, or when EPSV has an argument (EPSV ALL
         *         would forbid PASV for the rest of the session). Nothing may be sent then.
         */
        std::expected<std::string, RemoteIo::RemoteError> encode() const;

        /**
         * @brief The command for logs and transcripts, with the argument of PASS and ACCT hidden.
         */
        std::string toLogString() const;
    };
}
