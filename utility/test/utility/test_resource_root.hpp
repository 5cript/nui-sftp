#pragma once

#include <utility/resources.hpp>

#include <gtest/gtest.h>

#include <filesystem>

namespace Utility::Tests
{
    TEST(ResourceRootTests, ParentOfTheProgramDirectory)
    {
        EXPECT_EQ(resourceRoot("/opt/nui-sftp/bin"), std::filesystem::path{"/opt/nui-sftp"});
    }

    TEST(ResourceRootTests, ResourcesOfAMacAppBundle)
    {
        EXPECT_EQ(
            resourceRoot("/Applications/nui-sftp.app/Contents/MacOS"),
            std::filesystem::path{"/Applications/nui-sftp.app/Contents/Resources"}
        );
    }

    TEST(ResourceRootTests, PrefixWhenTheProgramIsNextToCef)
    {
        EXPECT_EQ(resourceRoot("/app/lib/nui-sftp"), std::filesystem::path{"/app"});
    }

    TEST(ResourceRootTests, OtherDirectoriesInLibStayTheParent)
    {
        EXPECT_EQ(resourceRoot("/usr/lib/other"), std::filesystem::path{"/usr/lib"});
    }
}
