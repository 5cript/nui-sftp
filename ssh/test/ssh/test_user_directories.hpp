#pragma once

#include <ssh/user_directories.hpp>

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace SecureShell::Test
{
    namespace
    {
        std::vector<std::pair<std::string, std::string>>
        parsed(std::string_view content, std::filesystem::path const& home = "/home/user")
        {
            std::vector<std::pair<std::string, std::string>> result;
            for (auto const& directory : parseUserDirectories(content, home))
                result.emplace_back(directory.name, directory.path.generic_string());
            return result;
        }

        using Entries = std::vector<std::pair<std::string, std::string>>;
    }

    TEST(UserDirectoriesTests, ExpandsHome)
    {
        EXPECT_EQ(
            parsed("XDG_DESKTOP_DIR=\"$HOME/Schreibtisch\"\nXDG_DOWNLOAD_DIR=\"$HOME/Downloads\"\n"),
            (Entries{{"DESKTOP", "/home/user/Schreibtisch"}, {"DOWNLOAD", "/home/user/Downloads"}})
        );
    }

    TEST(UserDirectoriesTests, KeepsAbsolutePaths)
    {
        EXPECT_EQ(parsed("XDG_MUSIC_DIR=\"/srv/music\""), (Entries{{"MUSIC", "/srv/music"}}));
    }

    TEST(UserDirectoriesTests, SkipsDirectoriesDisabledByPointingAtHome)
    {
        EXPECT_EQ(
            parsed("XDG_TEMPLATES_DIR=\"$HOME/\"\nXDG_PUBLICSHARE_DIR=\"$HOME\"\nXDG_MUSIC_DIR=\"/home/user/\""),
            Entries{}
        );
    }

    TEST(UserDirectoriesTests, SkipsCommentsBlankAndMalformedLines)
    {
        EXPECT_EQ(
            parsed(
                "# comment\n\n   \nXDG_DESKTOP_DIR=$HOME/Desktop\nDESKTOP_DIR=\"$HOME/Desktop\"\n"
                "XDG_DIR=\"$HOME/Desktop\"\nXDG_VIDEOS_DIR=\"Videos\"\nXDG_MUSIC_DIR=\"$HOMEMusic\"\n"
                "XDG_PICTURES_DIR=\"$HOME/Pictures\""
            ),
            (Entries{{"PICTURES", "/home/user/Pictures"}})
        );
    }

    TEST(UserDirectoriesTests, ToleratesWhitespaceAndCarriageReturns)
    {
        EXPECT_EQ(parsed("  XDG_DESKTOP_DIR = \"$HOME/Desktop\"  \r\n"), (Entries{{"DESKTOP", "/home/user/Desktop"}}));
    }

    TEST(UserDirectoriesTests, UnescapesQuotedCharacters)
    {
        EXPECT_EQ(
            parsed(R"(XDG_DOCUMENTS_DIR="$HOME/My \"Docs\"")"), (Entries{{"DOCUMENTS", R"(/home/user/My "Docs")"}})
        );
    }

    TEST(UserDirectoriesTests, HandlesRootAndTrailingSlashHomes)
    {
        EXPECT_EQ(parsed("XDG_DESKTOP_DIR=\"$HOME/Desktop\"", "/"), (Entries{{"DESKTOP", "/Desktop"}}));
        EXPECT_EQ(
            parsed("XDG_DESKTOP_DIR=\"$HOME/Desktop\"", "/home/user/"), (Entries{{"DESKTOP", "/home/user/Desktop"}})
        );
    }
}
