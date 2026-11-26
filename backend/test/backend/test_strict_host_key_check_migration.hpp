#pragma once

#include <persistence/state/state.hpp>

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace Test
{
    namespace
    {
        /**
         * @brief A state as earlier versions wrote it: the default SSH options verify strictly.
         */
        Persistence::State stateWithStrictDefaults()
        {
            Persistence::State state{};
            state.sshOptions["default"].strictHostKeyCheck = true;
            state.sshOptions["other"].strictHostKeyCheck = true;
            return state;
        }
    }

    TEST(StrictHostKeyCheckMigrationTests, TheDefaultSshOptionsAskAboutUnknownHostKeysAfterwards)
    {
        auto state = stateWithStrictDefaults();

        EXPECT_TRUE(state.migrateStrictHostKeyCheckToAsking());

        EXPECT_EQ(state.sshOptions["default"].strictHostKeyCheck, std::optional<bool>{false});
        ASSERT_TRUE(state.appliedMigrations.has_value());
        EXPECT_EQ(*state.appliedMigrations, std::vector<std::string>{"strictHostKeyCheckToAsking"});
    }

    TEST(StrictHostKeyCheckMigrationTests, OptionsTheUserMadeStayStrict)
    {
        auto state = stateWithStrictDefaults();

        state.migrateStrictHostKeyCheckToAsking();

        EXPECT_EQ(state.sshOptions["other"].strictHostKeyCheck, std::optional<bool>{true});
    }

    TEST(StrictHostKeyCheckMigrationTests, DefaultsThatDoNotSayStayThatWay)
    {
        Persistence::State state{};
        state.sshOptions["default"] = Persistence::SshOptions{};

        state.migrateStrictHostKeyCheckToAsking();

        EXPECT_FALSE(state.sshOptions["default"].strictHostKeyCheck.has_value());
    }

    TEST(StrictHostKeyCheckMigrationTests, StrictTurnedOnAfterTheMigrationIsKept)
    {
        auto state = stateWithStrictDefaults();
        state.migrateStrictHostKeyCheckToAsking();
        state.sshOptions["default"].strictHostKeyCheck = true;

        const auto reloaded = [&state]()
        {
            auto loaded = nlohmann::json(state).get<Persistence::State>();
            EXPECT_FALSE(loaded.migrateStrictHostKeyCheckToAsking());
            return loaded;
        }();

        EXPECT_EQ(reloaded.sshOptions.at("default").strictHostKeyCheck, std::optional<bool>{true});
    }

    TEST(StrictHostKeyCheckMigrationTests, BothMigrationsAreKeptAsApplied)
    {
        auto state = stateWithStrictDefaults();

        state.migrateTerminalRendererFromDom();
        state.migrateStrictHostKeyCheckToAsking();

        const std::vector<std::string> applied{"terminalRendererFromDom", "strictHostKeyCheckToAsking"};
        EXPECT_EQ(*state.appliedMigrations, applied);
    }
}
