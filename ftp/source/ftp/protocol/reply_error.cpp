#include <ftp/protocol/reply_error.hpp>
#include <ftp/protocol/protocol_error.hpp>
#include <ftp/protocol/reply_code.hpp>
#include "text.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <ranges>
#include <string>
#include <string_view>

namespace FileTransferProtocol
{
    namespace
    {
        using RemoteIo::RemoteErrorKind;

        /**
         * @brief @p text holds one of @p phrases, in any case.
         */
        bool mentionsAny(std::string_view text, std::initializer_list<std::string_view> phrases)
        {
            return std::ranges::any_of(
                phrases,
                [text](std::string_view phrase)
                {
                    return Detail::containsIgnoringCase(text, phrase);
                }
            );
        }

        bool mentionsTooMany(std::string_view text)
        {
            return mentionsAny(text, {"too many", "no more connections", "maximum number of"});
        }

        /**
         * @brief A ban after failed logins, such as "Too many failed login attempts".
         */
        bool mentionsLoginBan(std::string_view text)
        {
            return mentionsTooMany(text) && mentionsAny(text, {"fail", "attempt", "incorrect", "invalid"});
        }

        bool mentionsConnectionLimit(std::string_view text)
        {
            return mentionsTooMany(text) && !mentionsLoginBan(text);
        }

        /**
         * @brief A line of @p text ends with one of @p phrases in any case, before closing punctuation. Servers put an
         *        errno text after the path they echo, so the words of a path decide nothing.
         */
        bool aLineEndsWithAny(std::string_view text, std::initializer_list<std::string_view> phrases)
        {
            return std::ranges::any_of(
                std::views::split(text, '\n'),
                [phrases](auto const& range)
                {
                    const std::string_view line{range.begin(), range.end()};
                    const auto end = line.find_last_not_of(" .!;");
                    const auto body = end == std::string_view::npos ? std::string_view{} : line.substr(0u, end + 1u);
                    return std::ranges::any_of(
                        phrases,
                        [body](std::string_view phrase)
                        {
                            return Detail::endsWithIgnoringCase(body, phrase);
                        }
                    );
                }
            );
        }

        /**
         * @brief The kind an errno text or a fixed text names, as proftpd, pure-ftpd, IIS and FileZilla Server send
         *        them.
         */
        std::optional<RemoteErrorKind> kindNamedBy(std::string_view text)
        {
            if (aLineEndsWithAny(
                    text,
                    {"no such file or directory",
                        "no such file",
                        "no such directory",
                        "not found",
                        "does not exist",
                        "doesn't exist",
                        "cannot find the file specified",
                        "cannot find the path specified"}
                ))
                return RemoteErrorKind::NoSuchFile;
            if (aLineEndsWithAny(text, {"permission denied", "access denied", "access is denied", "not permitted"}))
                return RemoteErrorKind::PermissionDenied;
            if (aLineEndsWithAny(text, {"file exists", "already exists"}))
                return RemoteErrorKind::AlreadyExists;
            if (aLineEndsWithAny(text, {"no space left on device"}))
                return RemoteErrorKind::NoSpace;
            if (aLineEndsWithAny(text, {"quota exceeded"}))
                return RemoteErrorKind::QuotaExceeded;
            return std::nullopt;
        }

        /**
         * @brief A refused login that asks for TLS: vsftpd's "Non-anonymous sessions must use encryption", proftpd's
         *        "SSL/TLS required on the control channel", pure-ftpd's "cleartext sessions ... are not accepted",
         *        FileZilla Server's "does not allow plain FTP" and IIS' "Policy requires SSL".
         */
        bool demandsTls(std::optional<Verb> verb, std::string_view text)
        {
            const auto login = verb == Verb::User || verb == Verb::Password || verb == Verb::Account;
            return login &&
                mentionsAny(text, {"tls", "ssl", "encryption", "cleartext", "plain ftp", "secure connection"}) &&
                mentionsAny(text, {"must", "require", "not accepted", "not allow", "need"});
        }

        /**
         * @brief A transfer refused for the protection level of its data connection: 521 (RFC 4217 10.2), as vsftpd's
         *        "Data connections must be encrypted", or 534 (RFC 2228 6).
         */
        bool refusesDataProtection(int code, std::optional<Verb> verb)
        {
            const auto transfer = verb == Verb::Retrieve || verb == Verb::Store || verb == Verb::Append ||
                verb == Verb::List || verb == Verb::NameList || verb == Verb::MachineListDirectory;
            return transfer && (code == 521 || code == 534);
        }

        RemoteErrorKind kindOf(int code, std::string_view text, std::optional<Verb> verb)
        {
            if (demandsTls(verb, text) || refusesDataProtection(code, verb))
                return RemoteErrorKind::NoSecureConnection;
            switch (code)
            {
                case 421:
                    if (mentionsLoginBan(text))
                        return RemoteErrorKind::AuthenticationFailed;
                    if (mentionsConnectionLimit(text))
                        return RemoteErrorKind::ConnectionLimit;
                    return verb ? RemoteErrorKind::ConnectionLost : RemoteErrorKind::NoConnection;
                case 425:
                case 434:
                    return RemoteErrorKind::NoConnection;
                case 426:
                    return RemoteErrorKind::ConnectionLost;
                case 430:
                    return RemoteErrorKind::AuthenticationFailed;
                case 431:
                case 502:
                case 504:
                case 522:
                case 536:
                    return RemoteErrorKind::Unsupported;
                case 450:
                case 451:
                // RFC 959 Appendix II for MKD of an existing directory.
                case 521:
                case 550:
                    return kindNamedBy(text).value_or(RemoteErrorKind::Failure);
                case 452:
                    return RemoteErrorKind::NoSpace;
                case 500:
                    // vsftpd reports its fatal errors as "500 OOPS: ...".
                    if (Detail::equalIgnoringCase(text.substr(0u, 4u), "oops"))
                        return RemoteErrorKind::Failure;
                    return verb ? RemoteErrorKind::Unsupported : RemoteErrorKind::NoConnection;
                case 503:
                    return RemoteErrorKind::ProtocolViolation;
                case 530:
                    if (mentionsConnectionLimit(text))
                        return RemoteErrorKind::ConnectionLimit;
                    return RemoteErrorKind::AuthenticationFailed;
                case 532:
                case 533:
                case 534:
                    return RemoteErrorKind::PermissionDenied;
                case 552:
                    return RemoteErrorKind::QuotaExceeded;
                case 553:
                    return kindNamedBy(text).value_or(RemoteErrorKind::PermissionDenied);
                default:
                    return RemoteErrorKind::Failure;
            }
        }
    }

    RemoteIo::RemoteError replyError(Reply const& reply, std::optional<Verb> verb)
    {
        const auto replyClass = classifyReply(reply.code);
        const auto answered = verb ? Command{.verb = *verb}.toLogString() : std::string{"the greeting"};
        const auto text = reply.text();
        if (replyClass != ReplyClass::TransientNegative && replyClass != ReplyClass::PermanentNegative)
            return protocolViolation(
                fmt::format("Unexpected reply {} to {}: {}", reply.code, answered, text), reply.code
            );

        return RemoteIo::RemoteError{
            .kind = kindOf(reply.code, text, verb),
            .protocol = RemoteIo::Protocol::Ftp,
            .protocolCode = reply.code,
            .message = text,
        };
    }

    bool tellsOfMissingSessionReuse(Reply const& reply)
    {
        if (reply.code == 522)
            return true;
        if (reply.code != 425 && reply.code != 450)
            return false;
        const auto text = reply.text();
        return mentionsAny(text, {"session", "resum"}) || aLineEndsWithAny(text, {"operation not permitted"});
    }
}
