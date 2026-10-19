#pragma once

#include <persistence/state/state.hpp>

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include <map>
#include <string>

namespace Test
{
    namespace
    {
        Persistence::SessionOptions
        serverWithLayouts(Persistence::TerminalEngineType type, std::map<std::string, nlohmann::json> layouts)
        {
            auto session = Persistence::SessionOptions{};
            session.type = type;
            session.layouts = std::move(layouts);
            return session;
        }

        nlohmann::json layoutWith(std::string const& widget)
        {
            return nlohmann::json{{"main", {{"widgets", {widget}}}}};
        }
    }

    TEST(SessionLayoutsTests, LayoutsOfServersAreGatheredByType)
    {
        Persistence::State state{};
        state.sessions["server"] =
            serverWithLayouts(Persistence::TerminalEngineType::ssh, {{"files", layoutWith("file-explorer")}});
        state.sessions["shell"] =
            serverWithLayouts(Persistence::TerminalEngineType::shell, {{"wide", layoutWith("terminal")}});

        EXPECT_TRUE(state.gatherSessionLayouts());

        ASSERT_TRUE(state.sessionLayouts.has_value());
        EXPECT_EQ(state.sessionLayouts->ssh, (std::map<std::string, nlohmann::json>{{"files", layoutWith("file-explorer")}}));
        EXPECT_EQ(state.sessionLayouts->shell, (std::map<std::string, nlohmann::json>{{"wide", layoutWith("terminal")}}));
    }

    TEST(SessionLayoutsTests, SameNameAndLayoutIsMergedIntoOne)
    {
        Persistence::State state{};
        state.sessions["alpha"] =
            serverWithLayouts(Persistence::TerminalEngineType::ssh, {{"files", layoutWith("file-explorer")}});
        state.sessions["beta"] =
            serverWithLayouts(Persistence::TerminalEngineType::ssh, {{"files", layoutWith("file-explorer")}});

        state.gatherSessionLayouts();

        EXPECT_EQ(state.sessionLayouts->ssh.size(), 1u);
        EXPECT_TRUE(state.sessionLayouts->ssh.contains("files"));
    }

    TEST(SessionLayoutsTests, SameNameWithAnotherLayoutGetsTheServerName)
    {
        Persistence::State state{};
        state.sessions["alpha"] =
            serverWithLayouts(Persistence::TerminalEngineType::ssh, {{"files", layoutWith("file-explorer")}});
        state.sessions["beta"] = serverWithLayouts(Persistence::TerminalEngineType::ssh, {{"files", layoutWith("terminal")}});

        state.gatherSessionLayouts();

        auto const& layouts = state.sessionLayouts->ssh;
        ASSERT_EQ(layouts.size(), 2u);
        EXPECT_EQ(layouts.at("files"), layoutWith("file-explorer"));
        EXPECT_EQ(layouts.at("files (beta)"), layoutWith("terminal"));
    }

    TEST(SessionLayoutsTests, LayoutsOfServersStayForEarlierVersions)
    {
        Persistence::State state{};
        state.sessions["server"] =
            serverWithLayouts(Persistence::TerminalEngineType::ssh, {{"files", layoutWith("file-explorer")}});

        state.gatherSessionLayouts();

        ASSERT_TRUE(state.sessions["server"].layouts.has_value());
        EXPECT_EQ(state.sessions["server"].layouts->size(), 1u);
    }

    TEST(SessionLayoutsTests, GatheringRunsOnlyOnce)
    {
        Persistence::State state{};
        state.sessions["server"] =
            serverWithLayouts(Persistence::TerminalEngineType::ssh, {{"files", layoutWith("file-explorer")}});
        state.gatherSessionLayouts();
        state.sessionLayouts->ssh.erase("files");

        EXPECT_FALSE(state.gatherSessionLayouts());
        EXPECT_TRUE(state.sessionLayouts->ssh.empty());
    }

    TEST(SessionLayoutsTests, GatheredLayoutsSurviveSaving)
    {
        Persistence::State state{};
        state.sessions["server"] =
            serverWithLayouts(Persistence::TerminalEngineType::ssh, {{"files", layoutWith("file-explorer")}});
        state.gatherSessionLayouts();

        auto reloaded = nlohmann::json(state).get<Persistence::State>();

        ASSERT_TRUE(reloaded.sessionLayouts.has_value());
        EXPECT_EQ(reloaded.sessionLayouts->ssh, state.sessionLayouts->ssh);
        EXPECT_FALSE(reloaded.gatherSessionLayouts());
    }
}
