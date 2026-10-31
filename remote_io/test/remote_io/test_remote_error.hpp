#pragma once

#include <remote_io/remote_error.hpp>

#include <gtest/gtest.h>

namespace RemoteIo::Test
{
    TEST(RemoteErrorTests, ToStringNamesProtocolKindCodeMessageAndWrapperError)
    {
        const RemoteError error{
            .kind = RemoteErrorKind::NoSuchFile,
            .protocol = Protocol::Sftp,
            .protocolCode = 2,
            .message = "/home/user/missing.txt",
            .wrapperError = WrapperErrors::InvalidPath,
        };

        EXPECT_EQ(
            error.toString(), "SFTP error 'no such file' (#2): /home/user/missing.txt (wrapper error: invalid path)"
        );
    }

    TEST(RemoteErrorTests, ToStringLeavesOutWhatIsUnset)
    {
        EXPECT_EQ(RemoteError{}.toString(), "Remote error");
        EXPECT_EQ(
            (RemoteError{.kind = RemoteErrorKind::ConnectionLost, .protocol = Protocol::Ftp}.toString()),
            "FTP error 'connection lost'"
        );
        EXPECT_EQ(
            (RemoteError{.message = "The thread stopped", .wrapperError = WrapperErrors::TaskPushFailed}.toString()),
            "Remote error: The thread stopped (wrapper error: task push failed)"
        );
    }

    TEST(RemoteErrorTests, ProtocolsReadInCapitals)
    {
        EXPECT_EQ(toString(Protocol::Ssh), "SSH");
        EXPECT_EQ(toString(Protocol::Sftp), "SFTP");
        EXPECT_EQ(toString(Protocol::Ftp), "FTP");
        EXPECT_EQ(toString(Protocol::None), "Remote");
    }

    TEST(RemoteErrorTests, KindsAndWrapperErrorsReadAsWords)
    {
        EXPECT_EQ(toString(RemoteErrorKind::NoSuchFile), "no such file");
        EXPECT_EQ(toString(RemoteErrorKind::Busy), "busy");
        EXPECT_EQ(toString(WrapperErrors::UnexpectedEndOfFile), "unexpected end of file");
    }

    TEST(RemoteErrorTests, EveryKindHasAName)
    {
        const auto last = static_cast<int>(RemoteErrorKind::Failure);
        for (auto value = static_cast<int>(RemoteErrorKind::None); value <= last; ++value)
        {
            EXPECT_NE(toString(static_cast<RemoteErrorKind>(value)), "unknown kind") << "kind " << value;
        }
    }

    TEST(RemoteErrorTests, EveryWrapperErrorHasAName)
    {
        const auto last = static_cast<int>(WrapperErrors::InvalidPath);
        for (auto value = static_cast<int>(WrapperErrors::None); value <= last; ++value)
        {
            EXPECT_NE(toString(static_cast<WrapperErrors>(value)), "unknown wrapper error")
                << "wrapper error " << value;
        }
    }
}
