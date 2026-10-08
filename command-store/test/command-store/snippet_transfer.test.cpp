#include <command-store/snippet_transfer.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
    using CommandStore::TransferSnippet;
    using CommandStore::SnippetTransfer::ParseError;
    using CommandStore::SnippetTransfer::parse;
    using CommandStore::SnippetTransfer::toJson;

    class SnippetTransferTests : public ::testing::Test
    {};

    TEST_F(SnippetTransferTests, ExportedDocumentParsesBackIdentically)
    {
        const std::vector<TransferSnippet> snippets{
            {.name = "List", .command = "ls {{directory}}", .folder = "Files", .tags = {"quick"}, .favorite = true},
            {.name = "Uptime", .command = "uptime"},
        };
        const auto parsed = parse(toJson(snippets).dump(2));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, snippets);
    }

    TEST_F(SnippetTransferTests, ExportedDocumentCarriesTheVersion)
    {
        const auto document = toJson({{.name = "List", .command = "ls"}});
        EXPECT_EQ(document["version"], 1);
        EXPECT_EQ(document["snippets"].size(), 1u);
    }

    TEST_F(SnippetTransferTests, BareArrayIsAccepted)
    {
        const auto parsed = parse(R"([{"name": "List", "command": "ls"}, {"name": "Df", "command": "df -h"}])");
        ASSERT_TRUE(parsed.has_value());
        ASSERT_EQ(parsed->size(), 2u);
        EXPECT_EQ(parsed->back().command, "df -h");
    }

    TEST_F(SnippetTransferTests, SingleEntryObjectIsAccepted)
    {
        const auto parsed = parse(R"({"name": "List", "command": "ls", "folder": "Files"})");
        ASSERT_TRUE(parsed.has_value());
        ASSERT_EQ(parsed->size(), 1u);
        EXPECT_EQ(parsed->front().folder, "Files");
    }

    TEST_F(SnippetTransferTests, MissingVersionIsAccepted)
    {
        const auto parsed = parse(R"({"snippets": [{"name": "List", "command": "ls"}]})");
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->size(), 1u);
    }

    TEST_F(SnippetTransferTests, NewerVersionIsRejected)
    {
        const auto parsed = parse(R"({"version": 2, "snippets": []})");
        ASSERT_FALSE(parsed.has_value());
        EXPECT_EQ(parsed.error().error, ParseError::UnsupportedVersion);
        EXPECT_EQ(parsed.error().detail, "2");
    }

    TEST_F(SnippetTransferTests, InvalidJsonIsRejected)
    {
        const auto parsed = parse(R"({"snippets": [)");
        ASSERT_FALSE(parsed.has_value());
        EXPECT_EQ(parsed.error().error, ParseError::InvalidJson);
    }

    TEST_F(SnippetTransferTests, UnrelatedDocumentIsRejected)
    {
        for (auto const* text : {R"({"hello": "world"})", "42", R"({"snippets": {}})", R"(["ls"])"})
        {
            const auto parsed = parse(text);
            ASSERT_FALSE(parsed.has_value()) << text;
            EXPECT_EQ(parsed.error().error, ParseError::UnsupportedDocument) << text;
        }
    }

    TEST_F(SnippetTransferTests, EntryWithoutNameReportsItsIndex)
    {
        const auto parsed = parse(R"([{"name": "List", "command": "ls"}, {"name": "  ", "command": "df"}])");
        ASSERT_FALSE(parsed.has_value());
        EXPECT_EQ(parsed.error().error, ParseError::EntryWithoutName);
        EXPECT_EQ(parsed.error().entryIndex, 1u);
    }

    TEST_F(SnippetTransferTests, EntryWithoutCommandReportsItsIndex)
    {
        const auto parsed = parse(R"({"version": 1, "snippets": [{"name": "List", "command": ""}]})");
        ASSERT_FALSE(parsed.has_value());
        EXPECT_EQ(parsed.error().error, ParseError::EntryWithoutCommand);
        EXPECT_EQ(parsed.error().entryIndex, 0u);
    }

    TEST_F(SnippetTransferTests, NameAndFolderAreTrimmedAndOddFieldsIgnored)
    {
        const auto parsed = parse(
            R"([{"name": " List ", "command": " ls ", "folder": " Files ", "tags": ["a", 1, null, "b"], "favorite": "yes"}])"
        );
        ASSERT_TRUE(parsed.has_value());
        ASSERT_EQ(parsed->size(), 1u);
        auto const& snippet = parsed->front();
        EXPECT_EQ(snippet.name, "List");
        EXPECT_EQ(snippet.command, " ls ");
        EXPECT_EQ(snippet.folder, "Files");
        EXPECT_EQ(snippet.tags, (std::vector<std::string>{"a", "b"}));
        EXPECT_FALSE(snippet.favorite);
    }
}
