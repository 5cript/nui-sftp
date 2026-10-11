#pragma once

#include <persistence/legacy_migration.hpp>
#include <persistence/state/state.hpp>

#include <gtest/gtest.h>

namespace Test
{
    namespace
    {
        nlohmann::json sshSession(nlohmann::json engine)
        {
            return nlohmann::json{{"type", "ssh"}, {"engine", std::move(engine)}};
        }
    }

    TEST(LegacyMigrationTests, SingleUserAndKeysAreCopiedIntoTheFirstUser)
    {
        auto config = nlohmann::json{
            {"sessions",
                {{"server",
                    sshSession({
                        {"host", "example.com"},
                        {"user", "tim"},
                        {"sshKeyPrivate", "/keys/id"},
                        {"sshKeyPublic", "/keys/id.pub"},
                    })}}},
        };

        EXPECT_TRUE(Persistence::migrateLegacyEntries(config));

        auto const& engine = config["sessions"]["server"]["engine"];
        EXPECT_EQ(engine["user"], "tim");
        EXPECT_EQ(engine["sshKeyPrivate"], "/keys/id");
        EXPECT_EQ(engine["sshKeyPublic"], "/keys/id.pub");
        ASSERT_EQ(engine["users"].size(), 1u);
        EXPECT_EQ(engine["users"][0]["name"], "tim");
        EXPECT_EQ(engine["users"][0]["sshKeyPrivate"], "/keys/id");
        EXPECT_EQ(engine["users"][0]["sshKeyPublic"], "/keys/id.pub");
        EXPECT_EQ(engine["host"], "example.com");
    }

    TEST(LegacyMigrationTests, ServerWithoutUserGetsAnEmptyUserList)
    {
        auto config = nlohmann::json{{"sessions", {{"server", sshSession({{"host", "example.com"}})}}}};

        EXPECT_TRUE(Persistence::migrateLegacyEntries(config));
        EXPECT_EQ(config["sessions"]["server"]["engine"]["users"], nlohmann::json::array());
    }

    TEST(LegacyMigrationTests, CurrentEntriesAndLocalShellsAreLeftAlone)
    {
        auto config = nlohmann::json{
            {"sessions",
                {
                    {"server", sshSession({{"host", "example.com"}, {"users", {{{"name", "tim"}}}}})},
                    {"shell", {{"type", "shell"}, {"engine", {{"command", "/bin/bash"}}}}},
                }},
        };
        const auto before = config;

        EXPECT_FALSE(Persistence::migrateLegacyEntries(config));
        EXPECT_EQ(config, before);
    }

    TEST(LegacyMigrationTests, LastDirectConnectIsMigrated)
    {
        auto config = nlohmann::json{{"lastDirectConnect", {{"host", "10.0.0.1"}, {"user", "root"}}}};

        EXPECT_TRUE(Persistence::migrateLegacyEntries(config));
        EXPECT_EQ(config["lastDirectConnect"]["users"][0]["name"], "root");
    }

    TEST(LegacyMigrationTests, MigratedServerReadsIntoTheUserList)
    {
        auto config = sshSession({{"host", "example.com"}, {"user", "tim"}, {"sshKeyPrivate", "/keys/id"}});
        auto wrapped = nlohmann::json{{"sessions", {{"server", config}}}};
        ASSERT_TRUE(Persistence::migrateLegacyEntries(wrapped));

        const auto session = wrapped["sessions"]["server"].get<Persistence::SessionOptions>();
        auto const& ssh = std::get<Persistence::SshSessionOptions>(session.engine);
        ASSERT_EQ(ssh.users.size(), 1u);
        EXPECT_EQ(ssh.loginUser().name, "tim");
        EXPECT_EQ(ssh.loginUser().sshKeyPrivate, std::filesystem::path{"/keys/id"});
        EXPECT_FALSE(ssh.loginUser().sshKeyPublic.has_value());
    }

    TEST(LegacyMigrationTests, SelectedUserBecomesTheLoginUser)
    {
        Persistence::SshSessionOptions options{};
        options.users = {
            Persistence::SshUser{.name = "first"},
            Persistence::SshUser{.name = "second"},
            Persistence::SshUser{.name = "third"},
        };

        options.selectUser(2u);
        EXPECT_EQ(options.loginUser().name, "third");
        EXPECT_EQ(options.users[1].name, "first");
        EXPECT_EQ(options.users[2].name, "second");

        options.selectUser(5u);
        EXPECT_EQ(options.loginUser().name, "third");
    }

    TEST(LegacyMigrationTests, SavingWritesTheFirstUserForEarlierVersions)
    {
        Persistence::SshSessionOptions options{};
        options.users = {
            Persistence::SshUser{.name = "first", .sshKeyPrivate = "/keys/first"},
            Persistence::SshUser{.name = "second"},
        };
        options.writeLegacyUser();
        EXPECT_EQ(options.user, "first");
        EXPECT_EQ(options.sshKeyPrivate, std::filesystem::path{"/keys/first"});
        EXPECT_FALSE(options.sshKeyPublic.has_value());

        options.users.clear();
        options.writeLegacyUser();
        EXPECT_FALSE(options.user.has_value());
        EXPECT_FALSE(options.sshKeyPrivate.has_value());
    }

    TEST(LegacyMigrationTests, LegacyFieldsRoundTripWithoutDiff)
    {
        auto config = sshSession(
            {{"host", "example.com"},
                {"users", {{{"name", "tim"}}}},
                {"user", "tim"},
                {"maxReconnectAttempts", -1},
                {"maxReconnectBackoffMs", 16000},
                {"openSftpByDefault", true},
                {"remoteFavorites", nlohmann::json::array()},
                {"sshOptions", {{"$ref", "default"}}},
                {"sftpOptions", {{"$ref", "default"}}}}
        );
        const auto session = config.get<Persistence::SessionOptions>();
        const auto written = nlohmann::json(session);
        EXPECT_EQ(written["engine"]["user"], "tim");
        EXPECT_EQ(written["engine"]["users"], config["engine"]["users"]);
    }
}
