#include <command-store/snippet_presets.hpp>
#include <utility/command_template.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using CommandStore::SnippetPresets::parse;
    using CommandStore::SnippetTransfer::ParseError;

    class SnippetPresetsTests : public ::testing::Test
    {
      protected:
        static std::vector<std::filesystem::path> shippedPresets()
        {
            std::vector<std::filesystem::path> files{};
            for (auto const& entry : std::filesystem::directory_iterator{SNIPPET_PRESETS_DIRECTORY})
            {
                if (entry.path().extension() == ".json")
                    files.push_back(entry.path());
            }
            return files;
        }

        static std::string read(std::filesystem::path const& file)
        {
            const std::ifstream stream{file, std::ios::binary};
            std::stringstream content{};
            content << stream.rdbuf();
            return content.str();
        }
    };

    TEST_F(SnippetPresetsTests, PresetNamesTheFolderOfEverySnippet)
    {
        const auto preset = parse(R"({
            "version": 1,
            "preset": {"name": " Docker ", "icon": "shipping-status", "description": {"en_US": "Containers", "de_DE": "Container"}},
            "snippets": [{"name": "Running", "command": "docker ps", "folder": "Ignored"}]
        })");
        ASSERT_TRUE(preset.has_value());
        EXPECT_EQ(preset->name, "Docker");
        EXPECT_EQ(preset->icon, "shipping-status");
        ASSERT_EQ(preset->snippets.size(), 1u);
        EXPECT_EQ(preset->snippets[0].folder, "Docker");
    }

    TEST_F(SnippetPresetsTests, DescriptionFallsBackToEnglish)
    {
        const auto preset = parse(R"({
            "preset": {"name": "Git", "description": {"en_US": "Repositories", "de_DE": "Repositories auf Deutsch"}},
            "snippets": [{"name": "Status", "command": "git status"}]
        })");
        ASSERT_TRUE(preset.has_value());
        EXPECT_EQ(preset->description("de_DE"), "Repositories auf Deutsch");
        EXPECT_EQ(preset->description("fr_FR"), "Repositories");
    }

    TEST_F(SnippetPresetsTests, PlainTransferDocumentIsNoPreset)
    {
        const auto preset = parse(R"({"version": 1, "snippets": [{"name": "Status", "command": "git status"}]})");
        ASSERT_FALSE(preset.has_value());
        EXPECT_EQ(preset.error().error, ParseError::UnsupportedDocument);
    }

    TEST_F(SnippetPresetsTests, PresetWithoutSnippetsIsRejected)
    {
        const auto preset = parse(R"({"preset": {"name": "Empty"}, "snippets": []})");
        ASSERT_FALSE(preset.has_value());
        EXPECT_EQ(preset.error().error, ParseError::UnsupportedDocument);
    }

    TEST_F(SnippetPresetsTests, ShippedPresetsAreValid)
    {
        const auto files = shippedPresets();
        ASSERT_FALSE(files.empty());

        std::set<std::string> presetNames{};
        for (auto const& file : files)
        {
            SCOPED_TRACE(file.filename().string());
            const auto preset = parse(read(file));
            ASSERT_TRUE(preset.has_value());
            EXPECT_TRUE(presetNames.insert(preset->name).second) << "duplicate preset " << preset->name;
            EXPECT_FALSE(preset->icon.empty());
            EXPECT_TRUE(preset->descriptions.contains("en_US"));
            EXPECT_TRUE(preset->descriptions.contains("de_DE"));

            std::set<std::string> snippetNames{};
            for (auto const& snippet : preset->snippets)
            {
                SCOPED_TRACE(snippet.name);
                EXPECT_TRUE(snippetNames.insert(snippet.name).second) << "duplicate snippet name";
                EXPECT_FALSE(Utility::CommandTemplate::findUnmatchedBrace(snippet.command).has_value());
                EXPECT_TRUE(snippet.danger.has_value()) << "every shipped snippet is rated";
            }
        }
    }
}
