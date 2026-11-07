#pragma once

#include <ftp/support/server_fleet.hpp>
#include <ftp/support/server_profile.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

extern std::filesystem::path temporaryRoot;

namespace FileTransferProtocol::Test
{
    namespace
    {
        std::filesystem::path writeProfile(std::string_view content)
        {
            const auto directory = temporaryRoot / "server-profile";
            std::filesystem::create_directories(directory);
            const auto file = directory / "profile.json";
            std::ofstream{file} << content;
            return file;
        }
    }

    TEST(ServerProfileTests, EverySelectedServerHasAProfile)
    {
        const auto servers = selectedServers();
        ASSERT_FALSE(servers.empty());
        for (auto const& server : servers)
        {
            const auto profile = serverProfile(server);
            ASSERT_TRUE(profile.has_value()) << profile.error();
            EXPECT_EQ(profile->name, server);
            EXPECT_TRUE(std::filesystem::exists(profile->directory / "Dockerfile"));
            EXPECT_FALSE(profile->users.empty());
            for (auto const* const variant : {"default", "throttled", "limits"})
                EXPECT_TRUE(profile->variants.contains(variant)) << variant;
        }
    }

    TEST(ServerProfileTests, AProfileNamesItsServerAfterItsDirectory)
    {
        const auto profile = serverProfile("vsftpd");
        ASSERT_TRUE(profile.has_value()) << profile.error();
        EXPECT_EQ(profile->name, "vsftpd");
        EXPECT_EQ(profile->implicitPort, 990u);
        EXPECT_EQ(profile->expectations.missingReuse, MissingReuse::EndsSession);
        EXPECT_EQ(profile->expectations.abortReplies.plain, (std::vector<int>{426, 225}));
    }

    TEST(ServerProfileTests, AMissingFileIsAnError)
    {
        EXPECT_FALSE(loadServerProfile(temporaryRoot / "does-not-exist.json").has_value());
    }

    TEST(ServerProfileTests, BrokenJsonIsAnError)
    {
        const auto profile = loadServerProfile(writeProfile("{"));
        EXPECT_FALSE(profile.has_value());
    }

    TEST(ServerProfileTests, AMissingFieldIsAnError)
    {
        const auto profile = loadServerProfile(writeProfile(R"({"image": "only-this"})"));
        EXPECT_FALSE(profile.has_value());
    }

    TEST(ServerProfileTests, AnUnknownExpectationIsAnError)
    {
        const auto valid = serverProfile("vsftpd");
        ASSERT_TRUE(valid.has_value()) << valid.error();
        std::ifstream stream{valid->directory / "profile.json"};
        std::string text{std::istreambuf_iterator<char>{stream}, {}};
        const std::string known = R"("missingReuse": "endsSession")";
        text.replace(text.find(known), known.size(), R"("missingReuse": "shrugs")");

        const auto profile = loadServerProfile(writeProfile(text));
        ASSERT_FALSE(profile.has_value());
        EXPECT_NE(profile.error().find("missingReuse: unknown value 'shrugs'"), std::string::npos) << profile.error();
    }
}
