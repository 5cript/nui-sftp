#pragma once

#include <utility/fuzzy_search.hpp>

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace Utility::Tests
{
    using namespace ::testing;

    class FuzzySearchTests : public Test
    {
      protected:
        static FuzzySearch::SearchCandidate candidate(
            std::string_view label,
            std::string_view helpText = "",
            std::initializer_list<std::string_view> tags = {}
        )
        {
            FuzzySearch::SearchCandidate result{
                .label = FuzzySearch::NormalizedText::fromUtf8(label),
                .helpText = FuzzySearch::NormalizedText::fromUtf8(helpText),
            };
            for (const auto tag : tags)
                result.tags.push_back(FuzzySearch::NormalizedText::fromUtf8(tag));
            return result;
        }

        static std::vector<std::size_t> indicesOf(std::vector<FuzzySearch::RankedMatch> const& matches)
        {
            std::vector<std::size_t> result;
            for (auto const& match : matches)
                result.push_back(match.index);
            return result;
        }
    };

    TEST_F(FuzzySearchTests, LowercasesAcrossScripts)
    {
        EXPECT_EQ(FuzzySearch::normalize("GRÖSSE"), U"grösse");
        EXPECT_EQ(FuzzySearch::normalize("Größe"), U"größe");
        EXPECT_EQ(FuzzySearch::normalize("ΣΥΝΔΕΣΗ"), U"συνδεση");
        EXPECT_EQ(FuzzySearch::normalize("ПАРАЛЛЕЛЬНО Ё"), U"параллельно ё");
        EXPECT_EQ(FuzzySearch::normalize("ŁÓDŹ"), U"łódź");
        EXPECT_EQ(FuzzySearch::normalize("并发"), U"并发");
    }

    TEST_F(FuzzySearchTests, TokenizesMixedScripts)
    {
        const auto tokens = FuzzySearch::tokenize(FuzzySearch::normalize("SSH 并发传输，最大值: 5"));
        EXPECT_EQ(tokens, (std::vector<std::u32string>{U"ssh", U"并发传输", U"最大值", U"5"}));
    }

    TEST_F(FuzzySearchTests, FindsSynonymThroughTag)
    {
        const std::vector candidates{
            candidate("Compression Level", "Level of compression"),
            candidate("Concurrency", "Number of concurrent transfers.", {"parallel", "threads"}),
        };
        const auto matches = FuzzySearch::rank("parallel", candidates, 10u);
        ASSERT_EQ(matches.size(), 1u);
        EXPECT_EQ(matches[0].index, 1u);
        EXPECT_EQ(matches[0].field, FuzzySearch::MatchedField::Tag);
        EXPECT_EQ(matches[0].matchedTag, 0u);
    }

    TEST_F(FuzzySearchTests, ToleratesTypos)
    {
        const std::vector candidates{candidate("Concurrency"), candidate("Known Hosts File")};
        EXPECT_EQ(indicesOf(FuzzySearch::rank("concurency", candidates, 10u)), (std::vector<std::size_t>{0u}));
    }

    TEST_F(FuzzySearchTests, LabelHitOutranksHelpTextHit)
    {
        const std::vector candidates{
            candidate("Strict Host Key Check", "Verify the known hosts file before connecting."),
            candidate("Known Hosts File", "Path to the file."),
        };
        const auto matches = FuzzySearch::rank("known", candidates, 10u);
        ASSERT_EQ(matches.size(), 2u);
        EXPECT_EQ(matches[0].index, 1u);
        EXPECT_EQ(matches[0].field, FuzzySearch::MatchedField::Label);
        EXPECT_EQ(matches[1].field, FuzzySearch::MatchedField::HelpText);
    }

    TEST_F(FuzzySearchTests, RequiresEveryQueryToken)
    {
        const std::vector candidates{
            candidate("Show Hidden Files Locally"),
            candidate("Show Hidden Files Remotely"),
        };
        EXPECT_EQ(indicesOf(FuzzySearch::rank("hidden remote", candidates, 10u)), (std::vector<std::size_t>{1u}));
    }

    TEST_F(FuzzySearchTests, IgnoresQueriesThatAreTooShort)
    {
        const std::vector candidates{candidate("A setting"), candidate("Another one")};
        EXPECT_TRUE(FuzzySearch::rank("", candidates, 10u).empty());
        EXPECT_TRUE(FuzzySearch::rank("a", candidates, 10u).empty());
        EXPECT_TRUE(FuzzySearch::rank("  ", candidates, 10u).empty());
    }

    TEST_F(FuzzySearchTests, ShortTokensDoNotMatchLongQueries)
    {
        const std::vector candidates{candidate("Option", "A b c of an x")};
        EXPECT_TRUE(FuzzySearch::rank("parallel", candidates, 10u).empty());
    }

    TEST_F(FuzzySearchTests, ShortQueriesNeedCloseMatches)
    {
        const std::vector candidates{
            candidate("ECHO"),
            candidate("Prevent Create Directory"),
            candidate("Font Family", "Choose the font family."),
            candidate("IMAXBEL", "Echo BEL on input line too long"),
        };
        EXPECT_EQ(indicesOf(FuzzySearch::rank("echo", candidates, 10u)), (std::vector<std::size_t>{0u, 3u}));
    }

    TEST_F(FuzzySearchTests, SingleHanIdeographIsEnough)
    {
        const std::vector candidates{candidate("最大并发传输数"), candidate("主机")};
        EXPECT_EQ(indicesOf(FuzzySearch::rank("并", candidates, 10u)), (std::vector<std::size_t>{0u}));
        EXPECT_EQ(indicesOf(FuzzySearch::rank("并发", candidates, 10u)), (std::vector<std::size_t>{0u}));
    }

    TEST_F(FuzzySearchTests, MatchesUmlautsInBothDirections)
    {
        const std::vector candidates{candidate("Maximale Größe")};
        EXPECT_EQ(FuzzySearch::rank("GRÖSSE", candidates, 10u).size(), 1u);
        EXPECT_EQ(FuzzySearch::rank("größe", candidates, 10u).size(), 1u);
    }

    TEST_F(FuzzySearchTests, RespectsResultLimit)
    {
        std::vector<FuzzySearch::SearchCandidate> candidates;
        for (int number = 0; number < 25; ++number)
            candidates.push_back(candidate("Timeout " + std::to_string(number)));
        EXPECT_EQ(FuzzySearch::rank("timeout", candidates, 10u).size(), 10u);
        EXPECT_TRUE(FuzzySearch::rank("timeout", candidates, 0u).empty());
    }

    TEST_F(FuzzySearchTests, EqualScoresKeepCandidateOrder)
    {
        const std::vector candidates{
            candidate("Upload Concurrency"),
            candidate("Unrelated"),
            candidate("Download Concurrency"),
        };
        EXPECT_EQ(indicesOf(FuzzySearch::rank("concurrency", candidates, 10u)), (std::vector<std::size_t>{0u, 2u}));
    }

    TEST_F(FuzzySearchTests, PrefixOfLabelRanksFirst)
    {
        const std::vector candidates{candidate("Upload Concurrency"), candidate("Concurrency")};
        EXPECT_EQ(indicesOf(FuzzySearch::rank("concurrency", candidates, 10u)), (std::vector<std::size_t>{1u, 0u}));
    }

    TEST_F(FuzzySearchTests, BestPartialRatioStopsAtThreshold)
    {
        const auto itemTokens = FuzzySearch::tokenize(FuzzySearch::normalize("readme.md"));
        const auto queryTokens = FuzzySearch::tokenize(FuzzySearch::normalize("readme"));
        EXPECT_DOUBLE_EQ(FuzzySearch::bestPartialRatio(itemTokens, queryTokens, 90.), 100.);
    }
}
