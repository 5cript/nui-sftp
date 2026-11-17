#pragma once

#include <ftp/protocol/reply_error.hpp>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

namespace FileTransferProtocol::Test
{
    namespace
    {
        using RemoteIo::RemoteErrorKind;

        RemoteErrorKind kindOf(int code, std::string_view text = "Text", std::optional<Verb> verb = Verb::Retrieve)
        {
            return replyError(Reply{.code = code, .lines = {fmt::format("{} {}", code, text)}}, verb).kind;
        }
    }

    TEST(ReplyErrorTests, CodesOfRfc959)
    {
        EXPECT_EQ(kindOf(421), RemoteErrorKind::ConnectionLost);
        EXPECT_EQ(kindOf(425), RemoteErrorKind::NoConnection);
        EXPECT_EQ(kindOf(426), RemoteErrorKind::ConnectionLost);
        EXPECT_EQ(kindOf(450), RemoteErrorKind::Failure);
        EXPECT_EQ(kindOf(451), RemoteErrorKind::Failure);
        EXPECT_EQ(kindOf(452), RemoteErrorKind::NoSpace);
        EXPECT_EQ(kindOf(500), RemoteErrorKind::Unsupported);
        EXPECT_EQ(kindOf(501), RemoteErrorKind::Failure);
        EXPECT_EQ(kindOf(502), RemoteErrorKind::Unsupported);
        EXPECT_EQ(kindOf(503), RemoteErrorKind::ProtocolViolation);
        EXPECT_EQ(kindOf(504), RemoteErrorKind::Unsupported);
        EXPECT_EQ(kindOf(530), RemoteErrorKind::AuthenticationFailed);
        EXPECT_EQ(kindOf(532), RemoteErrorKind::PermissionDenied);
        EXPECT_EQ(kindOf(550), RemoteErrorKind::Failure);
        EXPECT_EQ(kindOf(551), RemoteErrorKind::Failure);
        EXPECT_EQ(kindOf(552), RemoteErrorKind::QuotaExceeded);
        EXPECT_EQ(kindOf(553), RemoteErrorKind::PermissionDenied);
    }

    TEST(ReplyErrorTests, CodesOfTheSecurityExtensions)
    {
        EXPECT_EQ(kindOf(431, "Unable to accept security mechanism", Verb::Authenticate), RemoteErrorKind::Unsupported);
        EXPECT_EQ(kindOf(522, "Network protocol not supported", Verb::ExtendedPassive), RemoteErrorKind::Unsupported);
        EXPECT_EQ(
            kindOf(533, "Command protection level denied", Verb::DataProtection), RemoteErrorKind::PermissionDenied
        );
        EXPECT_EQ(
            kindOf(534, "Request denied for policy reasons", Verb::Authenticate), RemoteErrorKind::PermissionDenied
        );
        EXPECT_EQ(kindOf(535, "Failed security check", Verb::DataProtection), RemoteErrorKind::Failure);
        EXPECT_EQ(
            kindOf(536, "Requested PROT level not supported", Verb::DataProtection), RemoteErrorKind::Unsupported
        );
    }

    TEST(ReplyErrorTests, LoginRefusedForWantOfTlsIsNoSecureConnection)
    {
        EXPECT_EQ(
            kindOf(530, "Non-anonymous sessions must use encryption.", Verb::User), RemoteErrorKind::NoSecureConnection
        );
        EXPECT_EQ(
            kindOf(550, "SSL/TLS required on the control channel", Verb::Password), RemoteErrorKind::NoSecureConnection
        );
        EXPECT_EQ(
            kindOf(
                421,
                "Sorry, cleartext sessions and weak ciphers are not accepted on this server.\n"
                "Please reconnect using TLS security mechanisms.",
                Verb::User
            ),
            RemoteErrorKind::NoSecureConnection
        );
        EXPECT_EQ(
            kindOf(530, "This server does not allow plain FTP. You have to use FTP over TLS.", Verb::User),
            RemoteErrorKind::NoSecureConnection
        );
        EXPECT_EQ(kindOf(534, "Policy requires SSL.", Verb::Account), RemoteErrorKind::NoSecureConnection);
    }

    TEST(ReplyErrorTests, OtherRefusalsThatNameTlsKeepTheirKind)
    {
        // vsftpd refuses anonymous logins over TLS, the session has TLS.
        EXPECT_EQ(
            kindOf(530, "Anonymous sessions may not use encryption.", Verb::User), RemoteErrorKind::AuthenticationFailed
        );
        EXPECT_EQ(kindOf(530, "User tls-backup cannot log in.", Verb::Password), RemoteErrorKind::AuthenticationFailed);
        EXPECT_EQ(kindOf(550, "SSL/TLS required on the data channel", Verb::Retrieve), RemoteErrorKind::Failure);
    }

    TEST(ReplyErrorTests, RefusalsThatTellOfAMissingSessionReuse)
    {
        const auto tells = [](int code, std::string_view text)
        {
            return tellsOfMissingSessionReuse(Reply{.code = code, .lines = {fmt::format("{} {}", code, text)}});
        };
        EXPECT_TRUE(tells(522, "SSL connection failed: session reuse required"));
        EXPECT_TRUE(tells(425, "Unable to build data connection: Operation not permitted"));
        EXPECT_TRUE(tells(425, "Unable to build data connection: TLS session of data connection not resumed."));
        EXPECT_TRUE(tells(
            450, "TLS session of data connection has not resumed or the session does not match the control connection"
        ));
        EXPECT_FALSE(tells(425, "Can't open data connection."));
        EXPECT_FALSE(tells(425, "Failed to establish connection."));
        EXPECT_FALSE(tells(450, "Requested file action not taken."));
        EXPECT_FALSE(tells(450, "Transfer aborted."));
        EXPECT_FALSE(tells(550, "session.log: Operation not permitted"));
        EXPECT_FALSE(tells(426, "Connection closed; transfer aborted."));
    }

    TEST(ReplyErrorTests, CodesOfOtherServers)
    {
        EXPECT_EQ(kindOf(430, "Invalid username or password", Verb::Password), RemoteErrorKind::AuthenticationFailed);
        EXPECT_EQ(kindOf(434, "Requested host unavailable", std::nullopt), RemoteErrorKind::NoConnection);
    }

    TEST(ReplyErrorTests, UnknownCodesUseTheirFirstDigit)
    {
        EXPECT_EQ(kindOf(499), RemoteErrorKind::Failure);
        EXPECT_EQ(kindOf(599), RemoteErrorKind::Failure);
        EXPECT_EQ(kindOf(455), RemoteErrorKind::Failure);
    }

    TEST(ReplyErrorTests, PositiveRepliesAreUnexpected)
    {
        for (const auto code : {110, 150, 200, 226, 257, 331, 350})
        {
            SCOPED_TRACE(code);
            const auto error = replyError(Reply{.code = code, .lines = {fmt::format("{} Fine", code)}}, Verb::Delete);
            EXPECT_EQ(error.kind, RemoteErrorKind::ProtocolViolation);
            EXPECT_EQ(error.protocolCode, code);
            EXPECT_NE(error.message.find("DELE"), std::string::npos);
        }
    }

    TEST(ReplyErrorTests, ErrnoTextsDecideAmbiguousCodes)
    {
        EXPECT_EQ(kindOf(550, "listing/missing.txt: No such file or directory"), RemoteErrorKind::NoSuchFile);
        EXPECT_EQ(kindOf(550, "Can't change directory to x: No such file or directory"), RemoteErrorKind::NoSuchFile);
        EXPECT_EQ(kindOf(550, "File not found"), RemoteErrorKind::NoSuchFile);
        EXPECT_EQ(kindOf(550, "secret.txt: Permission denied"), RemoteErrorKind::PermissionDenied);
        EXPECT_EQ(kindOf(550, "Access is denied."), RemoteErrorKind::PermissionDenied);
        EXPECT_EQ(kindOf(550, "x: Operation not permitted"), RemoteErrorKind::PermissionDenied);
        EXPECT_EQ(kindOf(550, "subdir: File exists", Verb::MakeDirectory), RemoteErrorKind::AlreadyExists);
        EXPECT_EQ(kindOf(450, "/missing: No such file or directory", Verb::List), RemoteErrorKind::NoSuchFile);
        EXPECT_EQ(kindOf(451, "Error during write: No space left on device"), RemoteErrorKind::NoSpace);
        EXPECT_EQ(kindOf(553, "Disk quota exceeded"), RemoteErrorKind::QuotaExceeded);
        EXPECT_EQ(kindOf(553, "Could not create file."), RemoteErrorKind::PermissionDenied);
    }

    TEST(ReplyErrorTests, WordsOfAnEchoedPathDecideNothing)
    {
        EXPECT_EQ(kindOf(550, "no such file.txt: Permission denied", Verb::Delete), RemoteErrorKind::PermissionDenied);
        EXPECT_EQ(kindOf(550, "Not Found Archive: Permission denied", Verb::Delete), RemoteErrorKind::PermissionDenied);
        EXPECT_EQ(kindOf(550, "quota: Is a directory", Verb::Delete), RemoteErrorKind::Failure);
        EXPECT_EQ(
            kindOf(550, "already exists: No such file or directory", Verb::Retrieve), RemoteErrorKind::NoSuchFile
        );
    }

    TEST(ReplyErrorTests, TextsOfWindowsServers)
    {
        EXPECT_EQ(kindOf(550, "The system cannot find the file specified. "), RemoteErrorKind::NoSuchFile);
        EXPECT_EQ(kindOf(550, "The system cannot find the path specified.", Verb::List), RemoteErrorKind::NoSuchFile);
        EXPECT_EQ(
            kindOf(550, "Cannot create a file when that file already exists.", Verb::MakeDirectory),
            RemoteErrorKind::AlreadyExists
        );
        EXPECT_EQ(kindOf(550, "Directory not found.", Verb::ChangeWorkingDirectory), RemoteErrorKind::NoSuchFile);
    }

    TEST(ReplyErrorTests, ExistingDirectoryOfRfc959AppendixII)
    {
        const Reply reply{
            .code = 521,
            .lines = {"521-\"/usr/dm/pathname\" directory already exists;", "521 taking no action."},
        };
        EXPECT_EQ(replyError(reply, Verb::MakeDirectory).kind, RemoteErrorKind::AlreadyExists);
    }

    TEST(ReplyErrorTests, TransferRefusedForItsDataProtectionIsNoSecureConnection)
    {
        EXPECT_EQ(
            kindOf(521, "Data connection cannot be opened with this PROT setting.", Verb::Retrieve),
            RemoteErrorKind::NoSecureConnection
        );
        EXPECT_EQ(kindOf(521, "Data connections must be encrypted.", Verb::List), RemoteErrorKind::NoSecureConnection);
        EXPECT_EQ(kindOf(534, "Request denied for policy reasons.", Verb::Store), RemoteErrorKind::NoSecureConnection);
        EXPECT_EQ(kindOf(534, "Request denied for policy reasons.", Verb::Delete), RemoteErrorKind::PermissionDenied);
    }

    TEST(ReplyErrorTests, TheCodeDecidesWhateverLanguageTheTextIsIn)
    {
        const Reply missing{.code = 550, .lines = {"550 Datei nicht gefunden"}};
        const auto error = replyError(missing, Verb::Retrieve);
        EXPECT_EQ(error.kind, RemoteErrorKind::Failure);
        EXPECT_EQ(error.protocolCode, 550);
        EXPECT_TRUE(error.message.contains("Datei nicht gefunden")) << error.toString();
        EXPECT_EQ(kindOf(452, "Espace disque insuffisant", Verb::Store), RemoteErrorKind::NoSpace);
        EXPECT_EQ(kindOf(530, "Anmeldung fehlgeschlagen", Verb::Password), RemoteErrorKind::AuthenticationFailed);
        EXPECT_EQ(kindOf(552, "Speicherplatz erschöpft", Verb::Store), RemoteErrorKind::QuotaExceeded);
    }

    TEST(ReplyErrorTests, TextsAreReadInAnyCase)
    {
        EXPECT_EQ(kindOf(550, "FILE NOT FOUND"), RemoteErrorKind::NoSuchFile);
        EXPECT_EQ(kindOf(550, "x: PERMISSION DENIED"), RemoteErrorKind::PermissionDenied);
        EXPECT_EQ(kindOf(421, "TOO MANY CONNECTIONS", std::nullopt), RemoteErrorKind::ConnectionLimit);
        EXPECT_EQ(kindOf(500, "oops: lower case", Verb::Noop), RemoteErrorKind::Failure);
    }

    TEST(ReplyErrorTests, FixedTextsLeaveAmbiguousCodesAsFailures)
    {
        // vsftpd sends one text per command, missing and denied look the same.
        EXPECT_EQ(kindOf(550, "Failed to open file.", Verb::Retrieve), RemoteErrorKind::Failure);
        EXPECT_EQ(kindOf(550, "Could not get file size.", Verb::Size), RemoteErrorKind::Failure);
        EXPECT_EQ(kindOf(550, "Create directory operation failed.", Verb::MakeDirectory), RemoteErrorKind::Failure);
    }

    TEST(ReplyErrorTests, ConnectionLimits)
    {
        EXPECT_EQ(
            kindOf(421, "There are too many connections from your internet address.", std::nullopt),
            RemoteErrorKind::ConnectionLimit
        );
        EXPECT_EQ(
            kindOf(421, "There are too many connected users, please try later.", std::nullopt),
            RemoteErrorKind::ConnectionLimit
        );
        EXPECT_EQ(kindOf(421, "Too many connections (3) from this IP", std::nullopt), RemoteErrorKind::ConnectionLimit);
        EXPECT_EQ(
            kindOf(
                530, "Sorry, the maximum number of clients (3) from your host are already connected.", Verb::Password
            ),
            RemoteErrorKind::ConnectionLimit
        );
        EXPECT_EQ(kindOf(530, "Login incorrect.", Verb::Password), RemoteErrorKind::AuthenticationFailed);
    }

    TEST(ReplyErrorTests, BansAfterFailedLoginsAreFailedAuthentication)
    {
        EXPECT_EQ(
            kindOf(421, "Temporarily banned for too many failed login attempts", Verb::Password),
            RemoteErrorKind::AuthenticationFailed
        );
        EXPECT_EQ(kindOf(421, "Too many authentication failures", std::nullopt), RemoteErrorKind::AuthenticationFailed);
        EXPECT_EQ(
            kindOf(530, "Maximum number of login attempts exceeded", Verb::Password),
            RemoteErrorKind::AuthenticationFailed
        );
    }

    TEST(ReplyErrorTests, RefusedGreetingIsNoConnection)
    {
        EXPECT_EQ(kindOf(421, "Service not available", std::nullopt), RemoteErrorKind::NoConnection);
        EXPECT_EQ(kindOf(500, "Unknown", std::nullopt), RemoteErrorKind::NoConnection);
    }

    TEST(ReplyErrorTests, IdleTimeoutIsALostConnection)
    {
        EXPECT_EQ(kindOf(421, "Timeout.", Verb::Noop), RemoteErrorKind::ConnectionLost);
        EXPECT_EQ(
            kindOf(421, "Login timeout (300 seconds): closing control connection", Verb::User),
            RemoteErrorKind::ConnectionLost
        );
    }

    TEST(ReplyErrorTests, FatalErrorsOfVsftpdAreFailures)
    {
        EXPECT_EQ(kindOf(500, "OOPS: vsf_sysutil_recv_peek", Verb::Noop), RemoteErrorKind::Failure);
    }

    TEST(ReplyErrorTests, ErrorCarriesCodeAndTextOfTheReply)
    {
        const Reply reply{.code = 550, .lines = {"550-First", " second\xFF", "550 Last"}};
        const auto error = replyError(reply, Verb::Delete);
        EXPECT_EQ(error.protocol, RemoteIo::Protocol::Ftp);
        EXPECT_EQ(error.protocolCode, 550);
        EXPECT_EQ(error.message, "First\n second\\xFF\nLast");
    }
}
