#pragma once

#include <utility/command_template.hpp>

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

namespace Utility::Tests
{
    using namespace ::testing;

    class CommandTemplateTests : public Test
    {};

    TEST_F(CommandTemplateTests, CommandWithoutVariablesHasNoVariables)
    {
        EXPECT_TRUE(CommandTemplate::parseVariables("ls -la /home").empty());
    }

    TEST_F(CommandTemplateTests, VariablesArePickedUpInOrderOfAppearance)
    {
        EXPECT_EQ(
            CommandTemplate::parseVariables("scp {{source}} {{user}}@{{host}}:{{target}}"),
            (std::vector<std::string>{"source", "user", "host", "target"})
        );
    }

    TEST_F(CommandTemplateTests, RepeatedVariablesAreReportedOnce)
    {
        EXPECT_EQ(
            CommandTemplate::parseVariables("cp {{file}} {{file}}.bak"), (std::vector<std::string>{"file"})
        );
    }

    TEST_F(CommandTemplateTests, WhitespaceWithinTheBracesIsIgnored)
    {
        EXPECT_EQ(CommandTemplate::parseVariables("echo {{  name\t}}"), (std::vector<std::string>{"name"}));
    }

    TEST_F(CommandTemplateTests, MalformedTokensAreNotVariables)
    {
        EXPECT_TRUE(CommandTemplate::parseVariables("echo {{}}").empty());
        EXPECT_TRUE(CommandTemplate::parseVariables("echo {{ }}").empty());
        EXPECT_TRUE(CommandTemplate::parseVariables("echo {{name}").empty());
        EXPECT_TRUE(CommandTemplate::parseVariables("echo {name}}").empty());
        EXPECT_TRUE(CommandTemplate::parseVariables("echo {{na me}}").empty());
        EXPECT_TRUE(CommandTemplate::parseVariables("echo {{na-me}}").empty());
        EXPECT_TRUE(CommandTemplate::parseVariables("echo {{").empty());
    }

    TEST_F(CommandTemplateTests, ShellBraceExpansionIsNotMistakenForAVariable)
    {
        EXPECT_TRUE(CommandTemplate::parseVariables("echo ${HOME} $(pwd) {a,b}").empty());
    }

    TEST_F(CommandTemplateTests, VariableIsFoundBehindALeadingExtraBrace)
    {
        EXPECT_EQ(CommandTemplate::parseVariables("echo {{{name}}"), (std::vector<std::string>{"name"}));
    }

    TEST_F(CommandTemplateTests, DigitsAndUnderscoresAreValidInNames)
    {
        EXPECT_EQ(CommandTemplate::parseVariables("echo {{my_var2}}"), (std::vector<std::string>{"my_var2"}));
    }

    TEST_F(CommandTemplateTests, SubstitutionReplacesEveryOccurrence)
    {
        EXPECT_EQ(
            CommandTemplate::substitute("cp {{file}} {{file}}.bak", {{"file", "notes.txt"}}),
            "cp notes.txt notes.txt.bak"
        );
    }

    TEST_F(CommandTemplateTests, SubstitutionIgnoresWhitespaceWithinTheBraces)
    {
        EXPECT_EQ(CommandTemplate::substitute("echo {{ name }}", {{"name", "world"}}), "echo world");
    }

    TEST_F(CommandTemplateTests, UnfilledVariablesAreKeptByDefault)
    {
        EXPECT_EQ(
            CommandTemplate::substitute("scp {{source}} {{target}}", {{"source", "a.txt"}}), "scp a.txt {{target}}"
        );
    }

    TEST_F(CommandTemplateTests, UnfilledVariablesAreDroppedWhenNotKept)
    {
        EXPECT_EQ(
            CommandTemplate::substitute("scp {{source}} {{target}}", {{"source", "a.txt"}}, false), "scp a.txt "
        );
    }

    TEST_F(CommandTemplateTests, UnknownValuesAreIgnored)
    {
        EXPECT_EQ(CommandTemplate::substitute("echo {{name}}", {{"other", "x"}, {"name", "hi"}}), "echo hi");
    }

    TEST_F(CommandTemplateTests, SubstitutedValuesAreNotRescanned)
    {
        EXPECT_EQ(
            CommandTemplate::substitute("echo {{outer}}", {{"outer", "{{inner}}"}, {"inner", "nope"}}),
            "echo {{inner}}"
        );
    }

    TEST_F(CommandTemplateTests, MalformedTokensSurviveSubstitution)
    {
        EXPECT_EQ(CommandTemplate::substitute("echo {{}} {{ }} ${HOME}", {{"", "x"}}), "echo {{}} {{ }} ${HOME}");
    }

    TEST_F(CommandTemplateTests, SubstitutionKeepsQuotesNewlinesAndUtf8)
    {
        EXPECT_EQ(
            CommandTemplate::substitute("echo \"{{text}}\"\nls", {{"text", "\xc3\xa4 $(pwd)"}}),
            "echo \"\xc3\xa4 $(pwd)\"\nls"
        );
    }

    TEST_F(CommandTemplateTests, EmptyCommandSubstitutesToEmpty)
    {
        EXPECT_EQ(CommandTemplate::substitute("", {{"name", "x"}}), "");
    }

    TEST_F(CommandTemplateTests, MatchedBracesAreNotReported)
    {
        EXPECT_EQ(CommandTemplate::findUnmatchedBrace(""), std::nullopt);
        EXPECT_EQ(CommandTemplate::findUnmatchedBrace("ls -la"), std::nullopt);
        EXPECT_EQ(CommandTemplate::findUnmatchedBrace("scp {{source}} {{ user }}@{{host}}:"), std::nullopt);
        EXPECT_EQ(CommandTemplate::findUnmatchedBrace("echo {{{name}}}"), std::nullopt);
    }

    TEST_F(CommandTemplateTests, UnclosedOpeningBracesAreReported)
    {
        EXPECT_EQ(CommandTemplate::findUnmatchedBrace("echo {{name"), 5u);
        EXPECT_EQ(CommandTemplate::findUnmatchedBrace("echo {{name}"), 5u);
        EXPECT_EQ(CommandTemplate::findUnmatchedBrace("echo {{"), 5u);
        EXPECT_EQ(CommandTemplate::findUnmatchedBrace("cp {{a}} {{b} {{c}}"), 9u);
    }

    TEST_F(CommandTemplateTests, HalfOpenedVariablesAreReported)
    {
        EXPECT_EQ(CommandTemplate::findUnmatchedBrace("echo {name}}"), 5u);
        EXPECT_EQ(CommandTemplate::findUnmatchedBrace("echo { name }} {{ok}}"), 5u);
    }

    TEST_F(CommandTemplateTests, NonVariableBracesInCommonCommandsAreNotReported)
    {
        EXPECT_EQ(CommandTemplate::findUnmatchedBrace("docker inspect -f '{{.State.Running}}' {{name}}"), std::nullopt);
        EXPECT_EQ(CommandTemplate::findUnmatchedBrace("curl -d '{\"a\":{\"b\":{}}}' {{url}}"), std::nullopt);
        EXPECT_EQ(CommandTemplate::findUnmatchedBrace("awk '{ if ($1 > 0) { print $1 }}'"), std::nullopt);
        EXPECT_EQ(CommandTemplate::findUnmatchedBrace("echo ${HOME} {a,b} $(pwd)"), std::nullopt);
    }
}
