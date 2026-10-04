#pragma once

#include <utility/echo_suppressor.hpp>
#include <utility/shell_integration.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace Utility::Tests
{
    using namespace ::testing;

    class EchoSuppressorTests : public Test
    {
      protected:
        /// Starts with a space like the real bootstrap, the most common byte in any output.
        const std::string line = " __nui_bootstrap(){ :; }";
        const std::string clearLine = "\r\x1b[2K";
        const std::string prompt = "user@host:~$ ";

        static std::string filterInChunks(EchoSuppressor& suppressor, std::vector<std::string_view> const& chunks)
        {
            std::string output;
            for (auto const chunk : chunks)
                output += suppressor.filter(chunk);
            return output;
        }

        static std::string filterBytewise(EchoSuppressor& suppressor, std::string_view stream)
        {
            std::string output;
            for (std::size_t index = 0; index < stream.size(); ++index)
                output += suppressor.filter(stream.substr(index, 1));
            return output;
        }

        /**
         * @brief Expects the same output for the stream in one piece, split in two at every position,
         *        and one byte at a time.
         */
        void expectChunkingIndependent(std::string const& stream, std::string const& expected, std::size_t passes)
        {
            {
                EchoSuppressor suppressor;
                suppressor.arm(line, passes, 100000);
                EXPECT_EQ(suppressor.filter(stream), expected) << "in one piece";
            }
            for (std::size_t split = 1; split < stream.size(); ++split)
            {
                EchoSuppressor suppressor;
                suppressor.arm(line, passes, 100000);
                const std::string_view view{stream};
                EXPECT_EQ(filterInChunks(suppressor, {view.substr(0, split), view.substr(split)}), expected)
                    << "split at " << split;
            }
            {
                EchoSuppressor suppressor;
                suppressor.arm(line, passes, 100000);
                EXPECT_EQ(filterBytewise(suppressor, stream), expected) << "byte by byte";
            }
        }
    };

    TEST_F(EchoSuppressorTests, UnarmedFilterPassesEverything)
    {
        EchoSuppressor suppressor;
        EXPECT_FALSE(suppressor.active());
        EXPECT_EQ(suppressor.filter(line + "\r\n"), line + "\r\n");
    }

    TEST_F(EchoSuppressorTests, EchoArrivingAloneIsSwallowed)
    {
        EchoSuppressor suppressor;
        suppressor.arm(line, 1, 100000);
        EXPECT_EQ(suppressor.filter(line + "\r\n"), clearLine);
        EXPECT_FALSE(suppressor.active());
    }

    TEST_F(EchoSuppressorTests, EchoAfterBannerAndPromptIsSwallowed)
    {
        // The first implementation disarmed on the banner and let the echo through.
        const std::string banner = "Welcome to Ubuntu 24.04 LTS\r\n\r\nLast login: Mon Oct  4 10:00:00 2026\r\n";
        expectChunkingIndependent(
            banner + prompt + line + "\r\n" + prompt + "ls\r\n", banner + prompt + clearLine + prompt + "ls\r\n", 1
        );
    }

    TEST_F(EchoSuppressorTests, BothEchoPassesAreSwallowed)
    {
        // The tty's canonical echo, then readline's redraw after the prompt.
        expectChunkingIndependent(
            line + "\r\n" + prompt + line + "\r\n" + prompt + "x", clearLine + prompt + clearLine + prompt + "x", 2
        );
    }

    TEST_F(EchoSuppressorTests, HeldPromptSpaceIsReleasedWhenTheOutputGoesQuiet)
    {
        // The line starts with a space, so the space ending a prompt looks like the start of the
        // echo while the filter is armed. The channel releases it once the output goes quiet,
        // instead of it waiting for the next keystroke.
        EchoSuppressor suppressor;
        suppressor.arm(line, 2, 100000);
        EXPECT_FALSE(suppressor.holding());
        EXPECT_EQ(suppressor.release(), "");

        EXPECT_EQ(suppressor.filter(line + "\r\n" + prompt), clearLine + prompt.substr(0, prompt.size() - 1));
        EXPECT_TRUE(suppressor.holding());
        EXPECT_EQ(suppressor.release(), " ");
        EXPECT_FALSE(suppressor.holding());
        EXPECT_TRUE(suppressor.active());

        // Still armed for the second echo; what follows it is untouched.
        EXPECT_EQ(suppressor.filter(line + "\r\n" + prompt + "ls"), clearLine + prompt + "ls");
        EXPECT_FALSE(suppressor.active());
    }

    TEST_F(EchoSuppressorTests, EchoInterleavedWithCursorMovementsIsSwallowed)
    {
        const auto middle = line.size() / 2;
        const auto noisy = line.substr(0, middle) + "\x1b[K\x1b[1C\x1b[3~" + line.substr(middle);
        expectChunkingIndependent(prompt + noisy + "\r\n" + prompt + "x", prompt + clearLine + prompt + "x", 1);
    }

    TEST_F(EchoSuppressorTests, EchoWrappedAcrossLinesIsSwallowed)
    {
        const auto middle = line.size() / 3;
        const auto wrapped = line.substr(0, middle) + "\r\n" + line.substr(middle, middle) + "\r" + line.substr(2 * middle);
        expectChunkingIndependent(prompt + wrapped + "\r\n" + prompt + "x", prompt + clearLine + prompt + "x", 1);
    }

    TEST_F(EchoSuppressorTests, ReadlineRepeatingACharacterAfterAWrapIsSwallowed)
    {
        // bash 3.2 and 4.4 print the character at the margin again after the carriage return.
        ASSERT_EQ(line[19], ' ');
        const auto wrapped = line.substr(0, 8) + "\r" + line.substr(7, 13) + "\r" + line.substr(19);
        expectChunkingIndependent(prompt + wrapped + "\r\n" + prompt + "x", prompt + clearLine + prompt + "x", 1);
    }

    TEST_F(EchoSuppressorTests, PaddingSpaceBeforeACursorMovementIsSwallowed)
    {
        // ZLE writes a space at the margin, moves the cursor and redraws the last character.
        const auto wrapped = line.substr(0, 10) + " \r\x1b[K" + line.substr(9, 1) + "\r" + line.substr(9);
        expectChunkingIndependent(prompt + wrapped + "\r\n" + prompt + "x", prompt + clearLine + prompt + "x", 1);
    }

    TEST_F(EchoSuppressorTests, CharacterwiseRedrawWithCursorPositioningIsSwallowed)
    {
        // fish draws typeahead one character at a time, each after a carriage return and a cursor
        // forward, with colors in between.
        std::string redraw;
        for (std::size_t index = 0; index < line.size(); ++index)
            redraw += "\r\x1b[" + std::to_string(14 + index) + "C\x1b[32m" + line.substr(index, 1) + "\x1b[m";
        // The prompt's trailing space cannot be told from a redraw of the echo's first one. It goes
        // with the echo, unseen: the cleared line takes the whole prompt line with it anyway.
        const auto promptWithoutSpace = prompt.substr(0, prompt.size() - 1);
        expectChunkingIndependent(prompt + redraw + "\r\n" + prompt + "x", promptWithoutSpace + clearLine + prompt + "x", 1);
    }

    TEST_F(EchoSuppressorTests, SpaceSkippedByACursorMovementAtAWordWrapIsSwallowed)
    {
        // fish wraps between words and moves over the blank cell instead of printing the space.
        const auto space = line.find(' ', 2);
        ASSERT_NE(space, std::string::npos);
        const auto wrapped = line.substr(0, space) + "\r\n\x1b[C" + line.substr(space + 1);
        expectChunkingIndependent(prompt + wrapped + "\r\n" + prompt + "x", prompt + clearLine + prompt + "x", 1);
    }

    TEST_F(EchoSuppressorTests, RepaintStartingPastTheLeadingBlankIsSwallowed)
    {
        // fish repaints the accepted line from its first visible character, the cursor already moved
        // past the leading space.
        const std::string before = "output\r\n\x1b[A\x1b[A\x1b[17D";
        expectChunkingIndependent(before + line.substr(1) + "\r\n" + prompt + "x", before + clearLine + prompt + "x", 1);
    }

    TEST_F(EchoSuppressorTests, SkippingTextThatIsNotBlankStillReleases)
    {
        const auto gap = line.substr(0, 8) + "\r\n\x1b[C" + line.substr(10) + "\r\n";
        expectChunkingIndependent(gap + prompt + "x", gap + prompt + "x", 1);
    }

    TEST_F(EchoSuppressorTests, StringSequencesInsideTheEchoAreSwallowed)
    {
        const auto middle = line.size() / 2;
        const auto marked = line.substr(0, middle) + "\x1b]133;B\x07\x1b]0;title with spaces\x1b\\" + line.substr(middle);
        expectChunkingIndependent(prompt + marked + "\r\n" + prompt + "x", prompt + clearLine + prompt + "x", 1);
    }

    TEST_F(EchoSuppressorTests, ModeSwitchBeforeTheLineEndIsSwallowedWhole)
    {
        // ZLE ends bracketed paste before the newline; none of it may leak as text.
        expectChunkingIndependent(line + "\x1b[?2004l\r\r\n" + prompt + "x", clearLine + prompt + "x", 1);
    }

    TEST_F(EchoSuppressorTests, PaddingSpaceFollowedByTextIsReleased)
    {
        const std::string almost = line.substr(0, 10) + " x and more\r\n";
        expectChunkingIndependent(almost + line + "\r\n" + prompt + "x", almost + clearLine + prompt + "x", 1);
    }

    TEST_F(EchoSuppressorTests, FalseStartIsPassedThroughIntactAndTheRealEchoIsStillCaught)
    {
        const std::string falseStart = " __nui_boot strap? no\r\n";
        expectChunkingIndependent(falseStart + line + "\r\n" + prompt + "x", falseStart + clearLine + prompt + "x", 1);
    }

    TEST_F(EchoSuppressorTests, StreamWithoutTheEchoComesThroughUnchanged)
    {
        // The invariant that matters most: a bug here silently deletes the user's output. A trailing
        // byte that cannot start the echo makes the filter decide about anything it still holds.
        const std::vector<std::string> streams{
            "plain output\r\n",
            "   lots   of   spaces   \r\n",
            " _\r\n __\r\n __nui\r\n __nui_bootstrap(){\r\n",
            "\x1b[1;32mcolored\x1b[0m output with \x1b]0;title\x07 sequences\r\n",
            "\xC3\xBC \xE6\x97\xA5 \xF0\x9F\x98\x80\r\n",
            std::string(5000, ' ') + "\r\n",
        };
        for (auto const& stream : streams)
        {
            const auto terminated = stream + "x";
            {
                EchoSuppressor suppressor;
                suppressor.arm(line, 2, 100000);
                EXPECT_EQ(suppressor.filter(terminated), terminated);
            }
            {
                EchoSuppressor suppressor;
                suppressor.arm(line, 2, 100000);
                EXPECT_EQ(filterBytewise(suppressor, terminated), terminated);
            }
        }
    }

    TEST_F(EchoSuppressorTests, BytesThatMightStartTheEchoWaitForTheNextChunk)
    {
        EchoSuppressor suppressor;
        suppressor.arm(line, 1, 100000);
        EXPECT_EQ(suppressor.filter(prompt), "user@host:~$");
        EXPECT_EQ(suppressor.filter("hello"), " hello");
    }

    TEST_F(EchoSuppressorTests, ExpiredBudgetReleasesEverythingAndDisarms)
    {
        // A shell with stty -echo, or a password prompt swallowing the line.
        EchoSuppressor suppressor;
        suppressor.arm(line, 2, 16);
        const std::string stream = "Password: " + line.substr(0, 10) + " and more output after the budget";
        EXPECT_EQ(suppressor.filter(stream), stream);
        EXPECT_FALSE(suppressor.active());
        EXPECT_EQ(suppressor.filter(line + "\r\n"), line + "\r\n");
    }

    TEST_F(EchoSuppressorTests, ExpiredBudgetMidChunkStillReleasesTheHeldCandidate)
    {
        EchoSuppressor suppressor;
        suppressor.arm(line, 2, 8);
        EXPECT_EQ(suppressor.filter("abc __n"), "abc");
        EXPECT_EQ(suppressor.filter("ui more"), " __nui more");
        EXPECT_FALSE(suppressor.active());
    }

    TEST_F(EchoSuppressorTests, LineEndOfTheEchoIsReplacedByExactlyOneClear)
    {
        for (auto const* ending : {"\n", "\r\n", "\r\r\n"})
        {
            EchoSuppressor suppressor;
            suppressor.arm(line, 1, 100000);
            EXPECT_EQ(suppressor.filter(line + ending + prompt), clearLine + prompt) << "ending size " << std::string{ending}.size();
        }
    }

    TEST_F(EchoSuppressorTests, MissingSecondPassDoesNotEatLaterOutput)
    {
        const std::string later = "total 0\r\ndrwxr-xr-x  2 user user  40 Oct  4 10:00 dir\r\n" + prompt + "x";
        expectChunkingIndependent(line + "\r\n" + later, clearLine + later, 2);
    }

    TEST_F(EchoSuppressorTests, RealBootstrapEchoIsSwallowed)
    {
        const auto bootstrap = " " + ShellIntegration::remoteBootstrap();
        EchoSuppressor suppressor;
        suppressor.arm(bootstrap);
        EXPECT_EQ(suppressor.filter(prompt + bootstrap + "\r\n" + prompt), prompt + clearLine + prompt.substr(0, prompt.size() - 1));
        EXPECT_TRUE(suppressor.active());
    }
}
