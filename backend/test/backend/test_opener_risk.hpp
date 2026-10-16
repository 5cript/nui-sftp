#pragma once

#include <backend/opener.hpp>
#include <backend/opener_risk.hpp>
#include <utility/temporary_directory.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    class OpenerRiskTests : public ::testing::Test
    {
      protected:
        std::filesystem::path write(std::string const& name, std::string const& content = "content")
        {
            const auto path = directory_.path() / name;
            std::ofstream{path, std::ios::binary} << content;
            return path;
        }

        static void makeExecutable(std::filesystem::path const& path)
        {
            std::filesystem::permissions(
                path, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add
            );
        }

        Opener opener_{};
        Utility::TemporaryDirectory directory_{temporaryRoot, true};
    };

    TEST_F(OpenerRiskTests, DocumentsAndFoldersAreNoRisk)
    {
        EXPECT_FALSE(opener_.riskOf(write("notes.txt")));
        EXPECT_FALSE(opener_.riskOf(directory_.path()));
    }

    TEST_F(OpenerRiskTests, ExtensionsCompareIgnoringCase)
    {
        EXPECT_TRUE(OpenerDetail::hasExtensionIn("/x/Setup.JAR", OpenerDetail::windowsRunnableExtensions));
        EXPECT_TRUE(OpenerDetail::hasExtensionIn("/x/tool.AppRef-MS", OpenerDetail::windowsRunnableExtensions));
        EXPECT_FALSE(OpenerDetail::hasExtensionIn("/x/notes.txt", OpenerDetail::windowsRunnableExtensions));
        EXPECT_FALSE(OpenerDetail::hasExtensionIn("/x/jar", OpenerDetail::windowsRunnableExtensions))
            << "a name without extension";
        EXPECT_FALSE(OpenerDetail::hasExtensionIn("/x/.jar", OpenerDetail::windowsRunnableExtensions))
            << "a dot file has no extension";
        EXPECT_FALSE(OpenerDetail::hasExtensionIn("/x/archive.jar.txt", OpenerDetail::windowsRunnableExtensions));
    }

    TEST_F(OpenerRiskTests, EveryPlatformListNamesItsRunnableTypes)
    {
        for (auto const* name : {"a.py", "a.pyw", "a.pyz", "a.application", "a.appx", "a.msix", "a.appxbundle",
                                 "a.msixbundle", "a.appinstaller", "a.settingcontent-ms"})
            EXPECT_TRUE(OpenerDetail::hasExtensionIn(name, OpenerDetail::windowsRunnableExtensions)) << name;
        for (auto const* name :
             {"a.inetloc", "a.fileloc", "a.prefpane", "a.saver", "a.mobileconfig", "a.terminal", "a.command"})
            EXPECT_TRUE(OpenerDetail::hasExtensionIn(name, OpenerDetail::macRunnableExtensions)) << name;
        for (auto const* name :
             {"a.desktop", "a.AppImage", "a.run", "a.jar", "a.deb", "a.rpm", "a.flatpakref", "a.flatpakrepo", "a.snap"})
            EXPECT_TRUE(OpenerDetail::hasExtensionIn(name, OpenerDetail::linuxRunnableExtensions)) << name;
    }

    TEST_F(OpenerRiskTests, ExecuteBitOfAnyClassCounts)
    {
        using std::filesystem::perms;
        EXPECT_FALSE(OpenerDetail::hasExecuteBit(perms::owner_read | perms::owner_write | perms::group_read));
        EXPECT_TRUE(OpenerDetail::hasExecuteBit(perms::owner_exec));
        EXPECT_TRUE(OpenerDetail::hasExecuteBit(perms::group_exec));
        EXPECT_TRUE(OpenerDetail::hasExecuteBit(perms::others_exec));
    }

    TEST_F(OpenerRiskTests, ExecutableContentIsElfOrInterpreterLine)
    {
        EXPECT_TRUE(OpenerDetail::hasExecutableContent(write("program", std::string{"\x7f" "ELF\x02\x01"})));
        EXPECT_TRUE(OpenerDetail::hasExecutableContent(write("script", "#!/bin/sh\necho hi\n")));
        EXPECT_FALSE(OpenerDetail::hasExecutableContent(write("text", "just words")));
        EXPECT_FALSE(OpenerDetail::hasExecutableContent(write("short", "#")));
        EXPECT_FALSE(OpenerDetail::hasExecutableContent(write("empty", "")));
        EXPECT_FALSE(OpenerDetail::hasExecutableContent(directory_.path() / "missing"));
    }

#ifndef _WIN32
    // Windows files carry no execute bit, every file reads as executable there.
    TEST_F(OpenerRiskTests, LinuxRuleNeedsProgramContentForTheExecuteBit)
    {
        const auto document = write("photo.jpg", "not a program");
        makeExecutable(document);
        EXPECT_FALSE(OpenerDetail::linuxRiskOf(document)) << "a mount that marks every file executable";

        const auto script = write("tool.txt", "#!/bin/sh\n");
        makeExecutable(script);
        EXPECT_EQ(OpenerDetail::linuxRiskOf(script), "backend.opener.risk.program");

        EXPECT_FALSE(OpenerDetail::linuxRiskOf(write("unmarked.sh", "#!/bin/sh\n")))
            << "a script without execute bit opens in an editor";
        EXPECT_EQ(OpenerDetail::linuxRiskOf(write("installer.deb")), "backend.opener.risk.dangerousType");
        EXPECT_EQ(OpenerDetail::linuxRiskOf(write("start.desktop")), "backend.opener.risk.dangerousType");
        EXPECT_FALSE(OpenerDetail::linuxRiskOf(directory_.path()));

        const auto link = directory_.path() / "readme.txt";
        std::filesystem::create_symlink(write("launcher.desktop"), link);
        EXPECT_EQ(OpenerDetail::linuxRiskOf(link), "backend.opener.risk.dangerousType") << "judged by its target too";
    }
#endif

#ifdef _WIN32
    TEST_F(OpenerRiskTests, ProgramsAndScriptsAreRisks)
    {
        EXPECT_EQ(opener_.riskOf(write("run.bat")), "backend.opener.risk.dangerousType");
        EXPECT_EQ(opener_.riskOf(write("run.vbs")), "backend.opener.risk.dangerousType");
        EXPECT_EQ(opener_.riskOf(write("tool.py")), "backend.opener.risk.dangerousType");
        EXPECT_EQ(opener_.riskOf(write("App.appref-ms")), "backend.opener.risk.dangerousType");
        // Judged by content, a renamed program is still one.
        const auto program = directory_.path() / "renamed.txt";
        std::filesystem::copy_file("C:\\Windows\\System32\\whoami.exe", program);
        EXPECT_EQ(opener_.riskOf(program), "backend.opener.risk.program");
    }
#else
    TEST_F(OpenerRiskTests, ProgramsAndScriptsAreRisks)
    {
        const auto script = write("tool.txt", "#!/bin/sh\n");
        makeExecutable(script);
        EXPECT_EQ(opener_.riskOf(script), "backend.opener.risk.program") << "the execute bit counts, not the name";
#    ifdef __APPLE__
        EXPECT_EQ(opener_.riskOf(write("start.command")), "backend.opener.risk.dangerousType");
        EXPECT_EQ(opener_.riskOf(write("site.inetloc")), "backend.opener.risk.dangerousType");
        const auto link = directory_.path() / "notes.txt";
        std::filesystem::create_symlink(write("start.terminal"), link);
        EXPECT_EQ(opener_.riskOf(link), "backend.opener.risk.dangerousType") << "a link is judged by its target";
#    else
        EXPECT_EQ(opener_.riskOf(write("start.desktop")), "backend.opener.risk.dangerousType");
        EXPECT_EQ(opener_.riskOf(write("package.rpm")), "backend.opener.risk.dangerousType");
#    endif
    }
#endif
}
