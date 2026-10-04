#pragma once

#include <utility/typed_line_buffer.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace Utility::Tests
{
    using namespace ::testing;

    class TypedLineBufferTests : public Test
    {
      protected:
        std::vector<std::string> lines{};
        TypedLineBuffer buffer{[this](std::string const& line) {
            lines.push_back(line);
        }};

        std::vector<std::string> linesFor(std::string_view keystrokes)
        {
            lines.clear();
            buffer.feed(keystrokes);
            return lines;
        }
    };

    TEST_F(TypedLineBufferTests, PlainLineIsEmittedOnEnter)
    {
        EXPECT_EQ(linesFor("ls -la\r"), (std::vector<std::string>{"ls -la"}));
    }

    TEST_F(TypedLineBufferTests, NothingIsEmittedBeforeEnter)
    {
        EXPECT_TRUE(linesFor("ls -la").empty());
        EXPECT_EQ(linesFor("\r"), (std::vector<std::string>{"ls -la"}));
    }

    TEST_F(TypedLineBufferTests, EveryLineEndingEmitsExactlyOnce)
    {
        EXPECT_EQ(linesFor("a\r"), (std::vector<std::string>{"a"}));
        EXPECT_EQ(linesFor("b\n"), (std::vector<std::string>{"b"}));
        EXPECT_EQ(linesFor("c\r\n"), (std::vector<std::string>{"c"}));
    }

    TEST_F(TypedLineBufferTests, EmptyLinesEmitNothing)
    {
        EXPECT_TRUE(linesFor("\r\r\n\n").empty());
    }

    TEST_F(TypedLineBufferTests, BackspaceRemovesTheLastCharacter)
    {
        EXPECT_EQ(linesFor("lss\x7f -l\r"), (std::vector<std::string>{"ls -l"}));
        EXPECT_EQ(linesFor("lss\b -l\r"), (std::vector<std::string>{"ls -l"}));
    }

    TEST_F(TypedLineBufferTests, BackspacePastTheStartIsHarmless)
    {
        EXPECT_EQ(linesFor("a\x7f\x7f\x7f" "b\r"), (std::vector<std::string>{"b"}));
    }

    TEST_F(TypedLineBufferTests, ControlKeysClearTheLine)
    {
        EXPECT_TRUE(linesFor("rm -rf /\x03\r").empty());
        EXPECT_TRUE(linesFor("exit\x04\r").empty());
        EXPECT_EQ(linesFor("wrong\x15right\r"), (std::vector<std::string>{"right"}));
    }

    TEST_F(TypedLineBufferTests, EscapeSequencesDropTheBuffer)
    {
        // Arrow up recalls history; what the shell runs is no longer what was typed.
        EXPECT_TRUE(linesFor("ls\x1b[A\r").empty());
        EXPECT_EQ(linesFor("\x1b[Apwd\r"), (std::vector<std::string>{"pwd"}));
        EXPECT_EQ(linesFor("\x1b[3~ok\r"), (std::vector<std::string>{"ok"}));
    }

    TEST_F(TypedLineBufferTests, BracketedPasteBelongsToTheLine)
    {
        EXPECT_EQ(linesFor("\x1b[200~git push\x1b[201~ --force\r"), (std::vector<std::string>{"git push --force"}));
        EXPECT_EQ(linesFor("ls \x1b[200~a\rb\x1b[201~\r"), (std::vector<std::string>{"ls a\nb"}));
    }

    TEST_F(TypedLineBufferTests, MultibyteInputSurvives)
    {
        EXPECT_EQ(
            linesFor("echo \xC3\xBCmlaut \xE6\x97\xA5\r"), (std::vector<std::string>{"echo \xC3\xBCmlaut \xE6\x97\xA5"})
        );
    }

    TEST_F(TypedLineBufferTests, BackspaceRemovesAWholeMultibyteCharacter)
    {
        EXPECT_EQ(linesFor("caf\xC3\xA9\x7f" "e\r"), (std::vector<std::string>{"cafe"}));
        EXPECT_EQ(linesFor("a\xF0\x9F\x98\x80\x7f\r"), (std::vector<std::string>{"a"}));
    }

    TEST_F(TypedLineBufferTests, ChunkingDoesNotChangeTheResult)
    {
        const std::string keystrokes = "git st\x7f\x7fstatus\r\x1b[Als\x15pwd\recho \xC3\xBC\r";
        const auto whole = linesFor(keystrokes);
        EXPECT_EQ(whole, (std::vector<std::string>{"git status", "pwd", "echo \xC3\xBC"}));

        lines.clear();
        for (const auto character : keystrokes)
            buffer.feed(std::string_view{&character, 1});
        EXPECT_EQ(lines, whole);
    }
}
