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

extern std::filesystem::path temporaryRoot;

namespace Test
{
    namespace
    {
        nlohmann::json ftpServerJson()
        {
            return nlohmann::json{
                {"type", "ftp"},
                {"icon", "folder"},
                {"engine",
                    {{"host", "ftp.example.com"},
                        {"port", 2121},
                        {"users", {{{"name", "alice"}}, {{"name", "bob"}, {"account", "billing"}}}},
                        {"tlsMode", "Implicit"},
                        {"remoteFavorites", {"/pub"}},
                        {"ftpOptions", {{"$ref", "slow"}}},
                        {"sftpOptions", {{"$ref", "default"}}}}},
            };
        }
    }

    TEST(FtpSessionOptionsTests, AnFtpSessionIsReadAndWrittenBack)
    {
        const auto before = nlohmann::json{{"sessions", {{"files", ftpServerJson()}}}};

        const auto state = before.get<Persistence::State>();

        EXPECT_TRUE(state.unreadSessions.empty());
        ASSERT_TRUE(state.sessions.contains("files"));
        auto const& session = state.sessions.at("files");
        EXPECT_EQ(session.type, Persistence::TerminalEngineType::ftp);
        ASSERT_TRUE(std::holds_alternative<Persistence::FtpSessionOptions>(session.engine));
        auto const& ftp = std::get<Persistence::FtpSessionOptions>(session.engine);
        EXPECT_EQ(ftp.host, "ftp.example.com");
        EXPECT_EQ(ftp.port, 2121);
        EXPECT_EQ(ftp.tlsMode, Persistence::FtpTlsMode::Implicit);
        ASSERT_EQ(ftp.users.size(), 2u);
        EXPECT_EQ(ftp.users[1].account, "billing");
        EXPECT_EQ(ftp.ftpOptions.ref(), "slow");

        const auto written = nlohmann::json(state);
        EXPECT_EQ(written["sessions"]["files"]["type"], "ftp");
        EXPECT_EQ(written["sessions"]["files"]["engine"]["tlsMode"], "Implicit");
        EXPECT_EQ(written["sessions"]["files"]["engine"]["ftpOptions"], (nlohmann::json{{"$ref", "slow"}}));
        EXPECT_EQ(written["sessions"]["files"]["engine"]["users"][1]["account"], "billing");
    }

    TEST(FtpSessionOptionsTests, ANewFtpSessionReferencesTheDefaultOptions)
    {
        const auto session = Persistence::SessionOptions::create("folder", Persistence::TerminalEngineType::ftp);

        EXPECT_EQ(session.type, Persistence::TerminalEngineType::ftp);
        ASSERT_TRUE(std::holds_alternative<Persistence::FtpSessionOptions>(session.engine));
        auto const& ftp = std::get<Persistence::FtpSessionOptions>(session.engine);
        EXPECT_EQ(ftp.ftpOptions.ref(), "default");
        EXPECT_EQ(ftp.sftpOptions.ref(), "default");
        EXPECT_EQ(ftp.tlsMode, Persistence::FtpTlsMode::Explicit);
    }

    TEST(FtpSessionOptionsTests, ThePortFollowsTheTlsModeUnlessOneIsSet)
    {
        Persistence::FtpSessionOptions options{};
        options.tlsMode = Persistence::FtpTlsMode::Plain;
        EXPECT_EQ(options.effectivePort(), 21);
        options.tlsMode = Persistence::FtpTlsMode::Explicit;
        EXPECT_EQ(options.effectivePort(), 21);
        options.tlsMode = Persistence::FtpTlsMode::Implicit;
        EXPECT_EQ(options.effectivePort(), 990);
        options.port = 2121;
        EXPECT_EQ(options.effectivePort(), 2121);
    }

    TEST(FtpSessionOptionsTests, TheSelectedUserLogsIn)
    {
        Persistence::FtpSessionOptions options{};
        EXPECT_FALSE(options.loginUser().name.has_value());

        options.users = {Persistence::FtpUser{.name = "alice"}, Persistence::FtpUser{.name = "bob"}};
        options.selectUser(1u);
        EXPECT_EQ(options.loginUser().name, "bob");
        options.selectUser(5u);
        EXPECT_EQ(options.loginUser().name, "bob");
    }

    TEST(FtpSessionOptionsTests, ReferencedOptionsFillWhatTheSessionLeavesUnset)
    {
        auto state = nlohmann::json{{"sessions", {{"files", ftpServerJson()}}}}.get<Persistence::State>();
        state.ftpOptions["slow"] = Persistence::FtpOptions{.commandTimeoutSeconds = 60, .maxConnections = 2};
        state.sftpOptions["default"] = Persistence::SftpOptions{.concurrency = 3};
        std::get<Persistence::FtpSessionOptions>(state.sessions.at("files").engine).ftpOptions.value().maxConnections =
            5;

        auto session = state.sessions.at("files");
        state.resolveReferencesOf(session);

        auto const& ftp = std::get<Persistence::FtpSessionOptions>(session.engine);
        EXPECT_EQ(ftp.ftpOptions.value().commandTimeoutSeconds, 60);
        EXPECT_EQ(ftp.ftpOptions.value().maxConnections, 5);
        EXPECT_EQ(ftp.sftpOptions.value().concurrency, 3);
    }

    TEST(FtpSessionOptionsTests, InheritablesPointAnFtpSessionAtTheirGroups)
    {
        auto session = Persistence::SessionOptions::create(std::nullopt, Persistence::TerminalEngineType::ftp);

        Persistence::InheritableReferences{.sftpOptions = "transfers", .ftpOptions = "slow"}.applyTo(session);

        auto const& ftp = std::get<Persistence::FtpSessionOptions>(session.engine);
        EXPECT_EQ(ftp.ftpOptions.ref(), "slow");
        EXPECT_EQ(ftp.sftpOptions.ref(), "transfers");
    }

    TEST(FtpSessionOptionsTests, LayoutsOfFtpServersAreGatheredApart)
    {
        Persistence::State state{};
        auto server = Persistence::SessionOptions::create(std::nullopt, Persistence::TerminalEngineType::ftp);
        server.layouts = std::map<std::string, nlohmann::json>{{"files", {{"main", "file-explorer"}}}};
        state.sessions["files"] = server;

        EXPECT_TRUE(state.gatherSessionLayouts());

        ASSERT_TRUE(state.sessionLayouts.has_value());
        EXPECT_TRUE(state.sessionLayouts->ftp.contains("files"));
        EXPECT_TRUE(state.sessionLayouts->ssh.empty());
        EXPECT_EQ(&state.sessionLayouts->of(Persistence::TerminalEngineType::ftp), &state.sessionLayouts->ftp);
    }

    TEST(FtpSessionOptionsTests, TheEngineHasTwoThreadsUnlessSetAndAlwaysOne)
    {
        Persistence::State state{};
        EXPECT_EQ(Persistence::ftpEngineThreadsOf(state), 2);
        state.ftpEngineThreads = 4;
        EXPECT_EQ(Persistence::ftpEngineThreadsOf(state), 4);
        state.ftpEngineThreads = 0;
        EXPECT_EQ(Persistence::ftpEngineThreadsOf(state), 1);
    }

#ifndef _WIN32
    TEST(FtpSessionOptionsTests, AConfigWithoutFtpOptionsGetsTheDefaultsWithoutAWarning)
    {
        const auto configHome = temporaryRoot / "ftp_session_options";
        const ScopedVariable configHomeVariable{"XDG_CONFIG_HOME", configHome.string()};
        const auto path = Nui::resolvePath(Constants::persistencePath);
        std::filesystem::create_directories(path.parent_path());
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
            return warnings;
        };
        // A first load writes every default, the config of a version before FTP is that without the FTP options.
        load();
        {
            std::ifstream reader{path, std::ios_base::binary};
            auto earlier = nlohmann::json::parse(reader);
            earlier.erase("ftpOptions");
            std::ofstream writer{path, std::ios_base::binary};
            writer << earlier.dump(4);
        }

        const auto warnings = load();

        EXPECT_TRUE(warnings.empty());
        ASSERT_TRUE(holder.stateCache().ftpOptions.contains("default"));
        EXPECT_EQ(holder.stateCache().ftpOptions.at("default").maxConnections, 4);
        std::ifstream reader{path, std::ios_base::binary};
        EXPECT_TRUE(nlohmann::json::parse(reader).contains("ftpOptions"));
    }
#endif
}
