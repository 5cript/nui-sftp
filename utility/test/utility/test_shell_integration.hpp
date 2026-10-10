#pragma once

#include <utility/shell_integration.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>

namespace Utility::Tests
{
    using namespace ::testing;
    using ShellIntegration::ShellKind;

    class ShellIntegrationTests : public Test
    {};

    TEST_F(ShellIntegrationTests, ExecuteSubcodeYieldsTheCommand)
    {
        EXPECT_EQ(ShellIntegration::commandFromOscPayload("E;git status"), std::optional<std::string>{"git status"});
    }

    TEST_F(ShellIntegrationTests, OtherSubcodesAreDropped)
    {
        for (auto const* payload : {"A", "B", "C", "D;0", "P;Cwd=/home/user", "e;ls", ";ls", ""})
            EXPECT_EQ(ShellIntegration::commandFromOscPayload(payload), std::nullopt) << payload;
    }

    TEST_F(ShellIntegrationTests, EmptyAndBlankCommandsAreDropped)
    {
        EXPECT_EQ(ShellIntegration::commandFromOscPayload("E;"), std::nullopt);
        EXPECT_EQ(ShellIntegration::commandFromOscPayload("E;   "), std::nullopt);
        EXPECT_EQ(ShellIntegration::commandFromOscPayload("E;\\x0a"), std::nullopt);
    }

    TEST_F(ShellIntegrationTests, SurroundingWhitespaceIsTrimmed)
    {
        EXPECT_EQ(ShellIntegration::commandFromOscPayload("E;  ls -la  "), std::optional<std::string>{"ls -la"});
    }

    TEST_F(ShellIntegrationTests, EscapesAreDecoded)
    {
        EXPECT_EQ(ShellIntegration::commandFromOscPayload("E;echo a\\x3bb"), std::optional<std::string>{"echo a;b"});
        EXPECT_EQ(ShellIntegration::commandFromOscPayload("E;echo \\\\"), std::optional<std::string>{"echo \\"});
        EXPECT_EQ(ShellIntegration::commandFromOscPayload("E;echo a\\\\\\\\b"), std::optional<std::string>{"echo a\\\\b"});
        EXPECT_EQ(
            ShellIntegration::commandFromOscPayload("E;printf a\\x0ab"), std::optional<std::string>{"printf a\nb"}
        );
        EXPECT_EQ(ShellIntegration::commandFromOscPayload("E;echo \\x41\\x4A"), std::optional<std::string>{"echo AJ"});
    }

    TEST_F(ShellIntegrationTests, EscapedBackslashBeforeAnXStaysABackslash)
    {
        EXPECT_EQ(
            ShellIntegration::commandFromOscPayload("E;echo \\\\x41"), std::optional<std::string>{"echo \\x41"}
        );
    }

    TEST_F(ShellIntegrationTests, MalformedEscapesStayVerbatim)
    {
        EXPECT_EQ(ShellIntegration::commandFromOscPayload("E;echo \\"), std::optional<std::string>{"echo \\"});
        EXPECT_EQ(ShellIntegration::commandFromOscPayload("E;echo \\xZZ"), std::optional<std::string>{"echo \\xZZ"});
        EXPECT_EQ(ShellIntegration::commandFromOscPayload("E;echo \\x4"), std::optional<std::string>{"echo \\x4"});
        EXPECT_EQ(ShellIntegration::commandFromOscPayload("E;echo a\\b"), std::optional<std::string>{"echo a\\b"});
    }

    TEST_F(ShellIntegrationTests, AnythingAfterAnUnescapedSemicolonIsTheNonceField)
    {
        // VS Code's own script appends a nonce: E;<escaped command>;<nonce>.
        EXPECT_EQ(
            ShellIntegration::commandFromOscPayload("E;echo a\\x3b b;f00dcafe"),
            std::optional<std::string>{"echo a; b"}
        );
        EXPECT_EQ(ShellIntegration::commandFromOscPayload("E;ls;"), std::optional<std::string>{"ls"});
    }

    TEST_F(ShellIntegrationTests, MultibyteCommandsSurvive)
    {
        EXPECT_EQ(
            ShellIntegration::commandFromOscPayload("E;echo \xC3\xBCmlaut \xE6\x97\xA5\xE6\x9C\xAC"),
            std::optional<std::string>{"echo \xC3\xBCmlaut \xE6\x97\xA5\xE6\x9C\xAC"}
        );
    }

    TEST_F(ShellIntegrationTests, OurOwnBootstrapIsNeverACommand)
    {
        EXPECT_EQ(ShellIntegration::commandFromOscPayload("E;__nui_preexec(){ :; }"), std::nullopt);
    }

    TEST_F(ShellIntegrationTests, ShellKindIsDetectedFromTheExecutable)
    {
        EXPECT_EQ(ShellIntegration::detectShellKind("/bin/bash"), ShellKind::Bash);
        EXPECT_EQ(ShellIntegration::detectShellKind("/usr/bin/zsh"), ShellKind::Zsh);
        EXPECT_EQ(ShellIntegration::detectShellKind("/usr/local/bin/fish"), ShellKind::Fish);
        EXPECT_EQ(ShellIntegration::detectShellKind("sh"), ShellKind::Sh);
        EXPECT_EQ(ShellIntegration::detectShellKind("bash.exe"), ShellKind::Bash);
        EXPECT_EQ(ShellIntegration::detectShellKind("C:/msys64/usr/bin/BASH.EXE"), ShellKind::Bash);
    }

    TEST_F(ShellIntegrationTests, ShGetsTheGuardedBootstrap)
    {
        // /bin/sh may be dash, which must not see the bash-only hook.
        EXPECT_EQ(ShellIntegration::bootstrap(ShellKind::Sh), ShellIntegration::remoteBootstrap());
    }

    TEST_F(ShellIntegrationTests, UnknownShellsAreUnknown)
    {
        EXPECT_EQ(ShellIntegration::detectShellKind("/bin/dash"), ShellKind::Unknown);
        EXPECT_EQ(ShellIntegration::detectShellKind("/usr/bin/pwsh"), ShellKind::Unknown);
        EXPECT_EQ(ShellIntegration::detectShellKind("/bin/zsh-5.9"), ShellKind::Unknown);
        EXPECT_EQ(ShellIntegration::detectShellKind(""), ShellKind::Unknown);
    }

    TEST_F(ShellIntegrationTests, BootstrapsAreSingleLines)
    {
        EXPECT_TRUE(ShellIntegration::bootstrap(ShellKind::Unknown).empty());
        for (auto const kind : {ShellKind::Bash, ShellKind::Sh, ShellKind::Zsh, ShellKind::Fish})
        {
            const auto line = ShellIntegration::bootstrap(kind);
            EXPECT_FALSE(line.empty());
            EXPECT_EQ(line.find('\n'), std::string::npos);
        }
        EXPECT_EQ(ShellIntegration::remoteBootstrap().find('\n'), std::string::npos);
    }

    TEST_F(ShellIntegrationTests, FishEchoesTheBootstrapOnceMoreThanTheOthers)
    {
        EXPECT_EQ(ShellIntegration::echoCount(ShellKind::Bash), 2u);
        EXPECT_EQ(ShellIntegration::echoCount(ShellKind::Zsh), 2u);
        EXPECT_EQ(ShellIntegration::echoCount(ShellKind::Sh), 2u);
        EXPECT_EQ(ShellIntegration::echoCount(ShellKind::Unknown), 2u);
        EXPECT_EQ(ShellIntegration::echoCount(ShellKind::Fish), 3u);
    }

    TEST_F(ShellIntegrationTests, RemoteBootstrapBodyHasNoSingleQuotes)
    {
        // The body lives inside eval '...'; one more quote would end it early.
        const auto line = ShellIntegration::remoteBootstrap();
        EXPECT_EQ(std::ranges::count(line, '\''), 2);
    }

    TEST_F(ShellIntegrationTests, EnvironmentBootstrapFitsTheMacTypeahead)
    {
        // A macOS tty drops typeahead past 1024 bytes, the typed line must stay far below that.
        for (auto const kind : {ShellKind::Bash, ShellKind::Sh, ShellKind::Zsh, ShellKind::Fish})
        {
            const auto line = ShellIntegration::environmentBootstrap(kind);
            EXPECT_FALSE(line.empty());
            EXPECT_LT(line.size(), 128u) << line;
            EXPECT_NE(line.find(ShellIntegration::bootstrapVariable), std::string::npos) << line;
        }
        EXPECT_TRUE(ShellIntegration::environmentBootstrap(ShellKind::Unknown).empty());
    }

    TEST_F(ShellIntegrationTests, EnvironmentBootstrapCarriesTheHistoryMarker)
    {
        // The bash bootstrap deletes the history line that mentions __nui_preexec, which the typed eval
        // does through the name of the variable.
        EXPECT_NE(ShellIntegration::environmentBootstrap(ShellKind::Bash).find("__nui_preexec"), std::string::npos);
    }

    TEST_F(ShellIntegrationTests, EnvironmentBootstrapUnsetsTheVariable)
    {
        EXPECT_NE(ShellIntegration::environmentBootstrap(ShellKind::Bash).find("unset __nui_preexec_bootstrap"), std::string::npos);
        EXPECT_NE(ShellIntegration::environmentBootstrap(ShellKind::Fish).find("set -e __nui_preexec_bootstrap"), std::string::npos);
    }
}
