#pragma once

#include <frontend/file_explorer/side_paths.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace FileExplorer::Test
{
    namespace SidePathsTestDetail
    {
        /**
         * @brief Makes local paths parse as @p platform while it exists.
         */
        class LocalPlatformScope
        {
          public:
            explicit LocalPlatformScope(Paths::LocalPlatform platform)
                : previous_{std::exchange(Paths::defaultLocalPlatform(), platform)}
            {}
            ~LocalPlatformScope()
            {
                Paths::defaultLocalPlatform() = previous_;
            }
            LocalPlatformScope(LocalPlatformScope const&) = delete;
            LocalPlatformScope& operator=(LocalPlatformScope const&) = delete;
            LocalPlatformScope(LocalPlatformScope&&) = delete;
            LocalPlatformScope& operator=(LocalPlatformScope&&) = delete;

          private:
            Paths::LocalPlatform previous_;
        };

        inline std::optional<std::string> remoteParent(std::string_view directory)
        {
            return parentOf<Paths::RemotePath>(directory);
        }

        inline std::optional<std::string> localParent(std::string_view directory)
        {
            return parentOf<Paths::LocalPath>(directory);
        }
    }

    TEST(SidePathsTests, RemoteParentIgnoresATrailingSeparator)
    {
        using namespace SidePathsTestDetail;
        EXPECT_EQ(remoteParent("/home/test"), "/home");
        EXPECT_EQ(remoteParent("/home/test/"), "/home");
        EXPECT_EQ(remoteParent("/home"), "/");
        EXPECT_EQ(remoteParent("/"), std::nullopt);
        EXPECT_EQ(remoteParent("relative"), std::nullopt);
        EXPECT_EQ(remoteParent(""), std::nullopt);
    }

    TEST(SidePathsTests, PosixLocalParentIgnoresATrailingSeparator)
    {
        using namespace SidePathsTestDetail;
        const LocalPlatformScope scope{Paths::LocalPlatform::Posix};
        EXPECT_EQ(localParent("/home/tim/"), "/home");
        EXPECT_EQ(localParent("/home"), "/");
        EXPECT_EQ(localParent("/"), std::nullopt);
    }

    TEST(SidePathsTests, WindowsLocalParentOfADriveChildIsTheDriveRoot)
    {
        using namespace SidePathsTestDetail;
        const LocalPlatformScope scope{Paths::LocalPlatform::Windows};
        EXPECT_EQ(localParent("C:/Users"), "C:/");
        EXPECT_EQ(localParent("c:\\Users\\tim\\"), "C:/Users");
        EXPECT_EQ(localParent("C:/"), std::nullopt);
        EXPECT_EQ(localParent("C:"), std::nullopt);
        EXPECT_EQ(localParent("//server/share/x"), "//server/share/");
        EXPECT_EQ(localParent("//server/share"), std::nullopt);
    }

    TEST(SidePathsTests, NavigationTargetDropsTheTrailingSeparator)
    {
        EXPECT_EQ(navigationTarget<Paths::RemotePath>("/home/test/"), "/home/test");
        EXPECT_EQ(navigationTarget<Paths::RemotePath>("/home//test/./x/../"), "/home/test");
        EXPECT_EQ(navigationTarget<Paths::RemotePath>("/"), "/");
        EXPECT_EQ(navigationTarget<Paths::RemotePath>("relative/dir/"), "relative/dir");
    }

    TEST(SidePathsTests, NavigationTargetOfALocalPathIsItsNormalForm)
    {
        {
            const SidePathsTestDetail::LocalPlatformScope scope{Paths::LocalPlatform::Posix};
            EXPECT_EQ(navigationTarget<Paths::LocalPath>("/home/tim/../x/"), "/home/x");
            EXPECT_EQ(navigationTarget<Paths::LocalPath>("/"), "/");
        }
        {
            const SidePathsTestDetail::LocalPlatformScope scope{Paths::LocalPlatform::Windows};
            EXPECT_EQ(navigationTarget<Paths::LocalPath>("C:"), "C:/");
            EXPECT_EQ(navigationTarget<Paths::LocalPath>("d:\\data\\"), "D:/data");
        }
    }

    TEST(SidePathsTests, TargetDirectoryOfADrop)
    {
        const auto directory = *Paths::RemotePath::make("/home/test");
        EXPECT_EQ(targetDirectoryOf(directory, std::nullopt).value().value(), "/home/test");
        EXPECT_EQ(targetDirectoryOf(directory, std::string{"docs"}).value().value(), "/home/test/docs");
        EXPECT_EQ(targetDirectoryOf(directory, std::string{".."}).value().value(), "/home");
        EXPECT_FALSE(targetDirectoryOf(Paths::RemotePath::root(), std::string{".."}));
        EXPECT_FALSE(targetDirectoryOf(directory, std::string{""}));
        EXPECT_FALSE(targetDirectoryOf(directory, std::string{"a/b"}));
        EXPECT_FALSE(targetDirectoryOf(directory, std::string{"."}));
    }

    TEST(SidePathsTests, LocalChildFollowsThePlatformOfTheDirectory)
    {
        const auto windowsDirectory = *Paths::LocalPath::fromUtf8("C:/data", Paths::LocalPlatform::Windows);
        EXPECT_FALSE(childOf(windowsDirectory, std::string_view{"CON"}));
        EXPECT_FALSE(childOf(windowsDirectory, std::string_view{"a\\b"}));
        EXPECT_EQ(childOf(windowsDirectory, std::string_view{"x"}).value().toUtf8(), "C:/data/x");

        const auto posixDirectory = *Paths::LocalPath::fromUtf8("/data", Paths::LocalPlatform::Posix);
        EXPECT_EQ(childOf(posixDirectory, std::string_view{"CON"}).value().toUtf8(), "/data/CON");
        EXPECT_EQ(targetDirectoryOf(posixDirectory, std::string{".."}).value().toUtf8(), "/");
    }
}
