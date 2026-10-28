#pragma once

#include "test_environment.hpp"

#include <persistence/state/state.hpp>
#include <persistence/state_holder.hpp>
#include <constants/persistence.hpp>

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>
#include <nui/backend/filesystem/special_paths.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <variant>
#include <vector>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    namespace
    {
        nlohmann::json sshServer(nlohmann::json engine)
        {
            return nlohmann::json{{"type", "ssh"}, {"engine", std::move(engine)}};
        }

        nlohmann::json ftpServer()
        {
            return nlohmann::json{
                {"type", "ftp"},
                {"icon", "folder"},
                {"engine", {{"host", "ftp.example.com"}, {"tlsMode", "explicit"}, {"ftpOptions", {{"$ref", "default"}}}}},
            };
        }

        std::vector<std::string> unreadNames(Persistence::State const& state)
        {
            std::vector<std::string> names{};
            for (auto const& [name, session] : state.unreadSessions)
                names.push_back(name);
            return names;
        }

        std::vector<Persistence::LoadWarning>
        warningsOfKind(Persistence::LoadWarnings const& warnings, Persistence::LoadWarningKind kind)
        {
            std::vector<Persistence::LoadWarning> matching{};
            for (auto const& warning : warnings)
            {
                if (warning.kind == kind)
                    matching.push_back(warning);
            }
            return matching;
        }
    }

    TEST(UnknownConfigEntriesTests, EngineTypeNamesAreReadExactly)
    {
        EXPECT_EQ(Persistence::terminalEngineTypeFromName("ssh"), Persistence::TerminalEngineType::ssh);
        EXPECT_EQ(Persistence::terminalEngineTypeFromName("shell"), Persistence::TerminalEngineType::shell);
        EXPECT_FALSE(Persistence::terminalEngineTypeFromName("ftp").has_value());
        EXPECT_FALSE(Persistence::terminalEngineTypeFromName("SSH").has_value());
        EXPECT_FALSE(Persistence::terminalEngineTypeFromName("").has_value());
    }

    TEST(UnknownConfigEntriesTests, EveryEngineTypeOfThisVersionIsRead)
    {
        auto sessions = nlohmann::json::object();
        boost::mp11::mp_for_each<boost::describe::describe_enumerators<Persistence::TerminalEngineType>>(
            [&sessions](auto descriptor)
            {
                sessions[descriptor.name] = {{"type", descriptor.name}, {"engine", nlohmann::json::object()}};
            }
        );

        const auto state = nlohmann::json{{"sessions", sessions}}.get<Persistence::State>();

        EXPECT_TRUE(state.unreadSessions.empty());
        ASSERT_EQ(state.sessions.size(), sessions.size());
        for (auto const& [name, session] : state.sessions)
        {
            EXPECT_EQ(Persistence::terminalEngineTypeFromName(name), session.type) << name;
            EXPECT_FALSE(std::holds_alternative<std::monostate>(session.engine)) << name;
        }
    }

    TEST(UnknownConfigEntriesTests, SessionsOfUnknownTypesAreSetAsideAndWrittenBack)
    {
        const auto before = nlohmann::json{
            {"sessions",
                {
                    {"server", sshServer({{"host", "example.com"}})},
                    {"files", ftpServer()},
                    {"broken", {{"engine", {{"host", "nowhere"}}}}},
                }},
        };

        const auto state = before.get<Persistence::State>();

        EXPECT_EQ(unreadNames(state), (std::vector<std::string>{"broken", "files"}));
        EXPECT_EQ(state.sessions.size(), 1u);
        EXPECT_TRUE(state.sessions.contains("server"));
        const auto written = nlohmann::json(state);
        EXPECT_EQ(written["sessions"]["files"], before["sessions"]["files"]);
        EXPECT_EQ(written["sessions"]["broken"], before["sessions"]["broken"]);
        EXPECT_EQ(written["sessions"]["server"]["engine"]["host"], "example.com");
    }

    TEST(UnknownConfigEntriesTests, UnreadSessionsSurviveTheWayThroughTheFrontend)
    {
        const auto backendState =
            nlohmann::json{{"sessions", {{"server", sshServer({{"host", "example.com"}})}, {"files", ftpServer()}}}}
                .get<Persistence::State>();

        auto frontendState = Persistence::State{};
        nlohmann::json::parse(nlohmann::json(backendState).dump()).get_to(frontendState);
        frontendState.sessions.at("server").icon = "changed";
        frontendState.sessions["added"] = Persistence::SessionOptions::create("terminal");
        const auto saved = nlohmann::json::parse(nlohmann::json(frontendState).dump()).get<Persistence::State>();

        const auto written = nlohmann::json(saved);
        EXPECT_EQ(written["sessions"]["files"], ftpServer());
        EXPECT_EQ(written["sessions"]["server"]["icon"], "changed");
        EXPECT_TRUE(written["sessions"].contains("added"));
    }

    TEST(UnknownConfigEntriesTests, ReadingReplacesTheUnreadSessionsOfAnEarlierRead)
    {
        auto state = nlohmann::json{{"sessions", {{"files", ftpServer()}}}}.get<Persistence::State>();

        nlohmann::json{{"sessions", {{"server", sshServer({{"host", "example.com"}})}}}}.get_to(state);

        EXPECT_TRUE(state.unreadSessions.empty());
        EXPECT_FALSE(nlohmann::json(state)["sessions"].contains("files"));
    }

    TEST(UnknownConfigEntriesTests, AConfigOfThisVersionKeepsNothingUnread)
    {
        const auto before = nlohmann::json(Persistence::State{});

        const auto state = before.get<Persistence::State>();

        EXPECT_TRUE(state.unreadSessions.empty());
        EXPECT_EQ(nlohmann::json(state), before);
    }

#ifndef _WIN32
    TEST(UnknownConfigEntriesTests, StateHolderKeepsUnreadSessionsAndWarnsOnce)
    {
        const auto configHome = temporaryRoot / "unknown_config_entries";
        const ScopedVariable configHomeVariable{"XDG_CONFIG_HOME", configHome.string()};
        const auto path = Nui::resolvePath(Constants::persistencePath);
        std::filesystem::create_directories(path.parent_path());
        {
            std::ofstream writer{path, std::ios_base::binary};
            writer << nlohmann::json{
                {"sessions", {{"files", ftpServer()}, {"server", sshServer({{"host", "example.com"}})}}},
            }.dump(4);
        }

        Persistence::StateHolder holder{};
        const auto load = [&holder]()
        {
            std::optional<std::string> loadError{"not loaded"};
            Persistence::LoadWarnings warnings{};
            holder.load(
                [&loadError, &warnings](
                    std::optional<std::string> const& error,
                    Persistence::StateHolder&,
                    Persistence::LoadWarnings const& loaded
                )
                {
                    loadError = error;
                    warnings = loaded;
                }
            );
            EXPECT_FALSE(loadError.has_value()) << *loadError;
            holder.clearWarnings();
            return warningsOfKind(warnings, Persistence::LoadWarningKind::SessionsKeptUnread);
        };

        const auto firstWarnings = load();
        ASSERT_EQ(firstWarnings.size(), 1u);
        EXPECT_EQ(firstWarnings.front().arguments, (std::vector<std::string>{"- files"}));
        EXPECT_EQ(holder.stateCache().sessions.size(), 1u);
        EXPECT_TRUE(holder.stateCache().sessions.contains("server"));
        EXPECT_TRUE(load().empty());

        holder.stateCache().sessions["server"].icon = "changed";
        std::optional<std::string> saveError{"not saved"};
        holder.save(
            [&saveError](std::optional<std::string> const& error)
            {
                saveError = error;
            }
        );
        ASSERT_FALSE(saveError.has_value()) << *saveError;

        std::ifstream reader{path, std::ios_base::binary};
        const auto written = nlohmann::json::parse(reader);
        EXPECT_EQ(written["sessions"]["files"], ftpServer());
        EXPECT_EQ(written["sessions"]["server"]["icon"], "changed");
    }
#endif
}
