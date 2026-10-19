#pragma once

#include <persistence/state/state.hpp>

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace Test
{
    namespace
    {
        Persistence::State stateWithRenderers(std::string const& profileRenderer, std::string const& sessionRenderer)
        {
            Persistence::State state{};
            state.terminalOptions["default"].renderer = profileRenderer;
            state.terminalOptions["unset"] = Persistence::TerminalOptions{};
            state.sessions["server"].terminalOptions.value().renderer = sessionRenderer;
            return state;
        }
    }

    TEST(TerminalRendererMigrationTests, MarksTheMigrationAsApplied)
    {
        auto state = stateWithRenderers("dom", "dom");

        EXPECT_TRUE(state.migrateTerminalRendererFromDom());

        ASSERT_TRUE(state.appliedMigrations.has_value());
        EXPECT_EQ(*state.appliedMigrations, std::vector<std::string>{"terminalRendererFromDom"});
    }

#ifdef __linux__
    TEST(TerminalRendererMigrationTests, MovesProfilesAndSessionsFromDomToWebgl)
    {
        auto state = stateWithRenderers("dom", "dom");

        state.migrateTerminalRendererFromDom();

        EXPECT_EQ(state.terminalOptions["default"].renderer, "webgl");
        EXPECT_FALSE(state.terminalOptions["unset"].renderer.has_value());
        EXPECT_EQ(state.sessions["server"].terminalOptions.value().renderer, "webgl");
    }
#else
    TEST(TerminalRendererMigrationTests, KeepsDomWhereItWasNeverTheDefault)
    {
        auto state = stateWithRenderers("dom", "dom");

        state.migrateTerminalRendererFromDom();

        EXPECT_EQ(state.terminalOptions["default"].renderer, "dom");
        EXPECT_EQ(state.sessions["server"].terminalOptions.value().renderer, "dom");
    }
#endif

    TEST(TerminalRendererMigrationTests, DomChosenAfterTheMigrationIsKept)
    {
        auto state = stateWithRenderers("webgl", "webgl");
        state.migrateTerminalRendererFromDom();
        state.terminalOptions["default"].renderer = "dom";
        state.sessions["server"].terminalOptions.value().renderer = "dom";

        EXPECT_FALSE(state.migrateTerminalRendererFromDom());

        EXPECT_EQ(state.terminalOptions["default"].renderer, "dom");
        EXPECT_EQ(state.sessions["server"].terminalOptions.value().renderer, "dom");
        EXPECT_EQ(state.appliedMigrations->size(), 1u);
    }

    TEST(TerminalRendererMigrationTests, AppliedMigrationsSurviveSaving)
    {
        auto state = stateWithRenderers("dom", "dom");
        state.migrateTerminalRendererFromDom();

        auto reloaded = nlohmann::json(state).get<Persistence::State>();
        reloaded.terminalOptions["default"].renderer = "dom";

        EXPECT_FALSE(reloaded.migrateTerminalRendererFromDom());
        EXPECT_EQ(reloaded.terminalOptions["default"].renderer, "dom");
    }

    TEST(TerminalRendererMigrationTests, MissingAppliedMigrationsIsNotReportedAsMissing)
    {
        auto state = nlohmann::json::object().get<Persistence::State>();

        const auto missing = state.collectMissingMembers(state);

        EXPECT_TRUE(std::ranges::none_of(
            missing,
            [](std::string const& name)
            {
                return name.contains("appliedMigrations");
            }
        ));
    }
}
