#pragma once

#include <persistence/state/session_options.hpp>

#include <gtest/gtest.h>

namespace Test
{
    namespace
    {
        Persistence::SshSessionOptions serverWithThreeUsers()
        {
            Persistence::SshSessionOptions options{};
            options.users = {
                Persistence::SshUser{.name = "first", .sshKeyPrivate = "/keys/first"},
                Persistence::SshUser{.name = "second"},
                Persistence::SshUser{.name = "third"},
            };
            return options;
        }
    }

    TEST(SshUserSelectionTests, ServerWithoutUsersLogsInWithoutUser)
    {
        const Persistence::SshSessionOptions options{};
        EXPECT_FALSE(options.loginUser().name.has_value());
        EXPECT_FALSE(options.loginUser().sshKeyPrivate.has_value());
    }

    TEST(SshUserSelectionTests, SelectedUserBecomesTheLoginUser)
    {
        auto options = serverWithThreeUsers();

        options.selectUser(2u);
        EXPECT_EQ(options.loginUser().name, "third");
        EXPECT_EQ(options.users[1].name, "first");
        EXPECT_EQ(options.users[2].name, "second");

        options.selectUser(5u);
        EXPECT_EQ(options.loginUser().name, "third");
    }

    TEST(SshUserSelectionTests, UserWithSameNameAndKeyIsSelected)
    {
        auto options = serverWithThreeUsers();

        options.selectUserLike(Persistence::SshUser{.name = "second", .sshKeyPublic = "/keys/ignored.pub"});
        ASSERT_EQ(options.users.size(), 3u);
        EXPECT_EQ(options.loginUser().name, "second");
        EXPECT_EQ(options.users[1].name, "first");
    }

    TEST(SshUserSelectionTests, UserThatNoLongerExistsIsAddedInFront)
    {
        auto options = serverWithThreeUsers();

        options.selectUserLike(Persistence::SshUser{.name = "first", .sshKeyPrivate = "/keys/other"});
        ASSERT_EQ(options.users.size(), 4u);
        EXPECT_EQ(options.loginUser().name, "first");
        EXPECT_EQ(options.loginUser().sshKeyPrivate, std::filesystem::path{"/keys/other"});
        EXPECT_EQ(options.users[1].sshKeyPrivate, std::filesystem::path{"/keys/first"});
    }
}
