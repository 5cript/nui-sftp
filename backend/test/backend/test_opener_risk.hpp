#pragma once

#include <backend/opener.hpp>
#include <utility/temporary_directory.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    class OpenerRiskTests : public ::testing::Test
    {
      protected:
        std::filesystem::path write(std::string const& name)
        {
            const auto path = directory_.path() / name;
            std::ofstream{path} << "content";
            return path;
        }

        Opener opener_{};
        Utility::TemporaryDirectory directory_{temporaryRoot, true};
    };

    TEST_F(OpenerRiskTests, DocumentsAndFoldersAreNoRisk)
    {
        EXPECT_FALSE(opener_.riskOf(write("notes.txt")));
        EXPECT_FALSE(opener_.riskOf(directory_.path()));
    }

#ifdef _WIN32
    TEST_F(OpenerRiskTests, ProgramsAndScriptsAreRisks)
    {
        EXPECT_EQ(opener_.riskOf(write("run.bat")), "backend.opener.risk.dangerousType");
        EXPECT_EQ(opener_.riskOf(write("run.vbs")), "backend.opener.risk.dangerousType");
        // Judged by content, a renamed program is still one.
        const auto program = directory_.path() / "renamed.txt";
        std::filesystem::copy_file("C:\\Windows\\System32\\whoami.exe", program);
        EXPECT_EQ(opener_.riskOf(program), "backend.opener.risk.program");
    }
#else
    TEST_F(OpenerRiskTests, ProgramsAndScriptsAreRisks)
    {
        const auto script = write("tool.txt");
        std::filesystem::permissions(script, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
        EXPECT_EQ(opener_.riskOf(script), "backend.opener.risk.program") << "the execute bit counts, not the name";
#    ifdef __APPLE__
        EXPECT_EQ(opener_.riskOf(write("start.command")), "backend.opener.risk.dangerousType");
#    else
        EXPECT_EQ(opener_.riskOf(write("start.desktop")), "backend.opener.risk.dangerousType");
#    endif
    }
#endif
}
