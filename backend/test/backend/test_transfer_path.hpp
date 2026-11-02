#pragma once

#include <backend/sftp/transfer_path.hpp>
#include <constants/sftp.hpp>

#include <gtest/gtest.h>

namespace Test
{
    TEST(TransferPathTests, ASmallFileTakesTheSynchronousPath)
    {
        EXPECT_FALSE(takesAsynchronousPath(RemoteIo::Capabilities{}, 1u));
    }

    TEST(TransferPathTests, ABigFileTakesTheAsynchronousPath)
    {
        EXPECT_TRUE(takesAsynchronousPath(RemoteIo::Capabilities{}, Constants::bigFileCutOff));
    }

    TEST(TransferPathTests, EveryFileTakesTheAsynchronousPathOnAFileSystemThatPrefersIt)
    {
        const RemoteIo::Capabilities capabilities{.preferAsyncTransfers = true};
        EXPECT_TRUE(takesAsynchronousPath(capabilities, 0u));
        EXPECT_TRUE(takesAsynchronousPath(capabilities, 1u));
    }
}
