#pragma once

#include <backend/program_options.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace Test
{
    namespace
    {
        std::expected<ProgramOptions, ProgramOptionsExit> parse(std::vector<char const*> arguments)
        {
            arguments.insert(arguments.begin(), "nui-sftp");
            return parseProgramOptions(static_cast<int>(arguments.size()), arguments.data());
        }
    }

    TEST(ProgramOptionsTests, NoArgumentsStartTheProgram)
    {
        EXPECT_TRUE(parse({}).has_value());
        EXPECT_TRUE(parse({"--console"}).has_value());
        EXPECT_TRUE(parse({"--enable-dev-tools"}).has_value());
    }

    TEST(ProgramOptionsTests, HelpIsPrintedWithoutError)
    {
        const auto result = parse({"--help"});
        ASSERT_FALSE(result.has_value());
        EXPECT_FALSE(result.error().isError);
        EXPECT_NE(result.error().message.find("--enable-dev-tools"), std::string::npos);
    }

    TEST(ProgramOptionsTests, UnknownOptionIsAnErrorWithTheHelp)
    {
        const auto result = parse({"--hepl"});
        ASSERT_FALSE(result.has_value());
        EXPECT_TRUE(result.error().isError);
        EXPECT_NE(result.error().message.find("--hepl"), std::string::npos);
        EXPECT_NE(result.error().message.find("--console"), std::string::npos) << "the help follows the error";
    }
}
