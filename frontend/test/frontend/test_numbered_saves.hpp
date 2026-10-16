#pragma once

#include <frontend/session_components/numbered_saves.hpp>

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

namespace Test
{
    using SessionInternal::NumberedSaves;
    using SessionInternal::WriteOutcome;

    namespace
    {
        /**
         * @brief Runs @p saves against a directory holding @p existing, failing the writes of @p unwritable.
         */
        std::vector<std::size_t>
        runSaves(NumberedSaves& saves, std::set<std::string> existing, std::set<std::string> const& unwritable = {})
        {
            std::vector<std::size_t> attemptedContents;
            while (const auto attempt = saves.next())
            {
                attemptedContents.push_back(attempt->content);
                if (unwritable.contains(attempt->name))
                    saves.record(WriteOutcome::Failed);
                else if (!existing.insert(attempt->name).second)
                    saves.record(WriteOutcome::Exists);
                else
                    saves.record(WriteOutcome::Written);
            }
            return attemptedContents;
        }
    }

    TEST(NumberedSavesTests, ContentsGoToConsecutiveNames)
    {
        NumberedSaves saves{"log", ".txt", 3};
        runSaves(saves, {});
        EXPECT_EQ(saves.written(), (std::vector<std::string>{"log_0.txt", "log_1.txt", "log_2.txt"}));
        EXPECT_TRUE(saves.failures().empty());
    }

    TEST(NumberedSavesTests, ExistingNamesAreSkippedForTheSameContent)
    {
        NumberedSaves saves{"log", ".txt", 2};
        const auto attempted = runSaves(saves, {"log_0.txt", "log_2.txt"});
        EXPECT_EQ(saves.written(), (std::vector<std::string>{"log_1.txt", "log_3.txt"}));
        EXPECT_EQ(attempted, (std::vector<std::size_t>{0, 0, 1, 1}));
    }

    TEST(NumberedSavesTests, AttemptsAreCountedPerContent)
    {
        NumberedSaves saves{"log", "", 2, 3};
        runSaves(saves, {"log_0", "log_1", "log_2", "log_4", "log_5"});
        ASSERT_EQ(saves.failures().size(), 1u);
        EXPECT_EQ(saves.failures()[0].content, 0u);
        EXPECT_EQ(saves.failures()[0].name, "log_2");
        EXPECT_TRUE(saves.failures()[0].noFreeName);
        EXPECT_EQ(saves.written(), (std::vector<std::string>{"log_3"})) << "the second content gets its own attempts";
    }

    TEST(NumberedSavesTests, FailedWritesAreReportedAndTheRestContinues)
    {
        NumberedSaves saves{"log", ".txt", 2};
        runSaves(saves, {}, {"log_0.txt"});
        ASSERT_EQ(saves.failures().size(), 1u);
        EXPECT_EQ(saves.failures()[0].content, 0u);
        EXPECT_EQ(saves.failures()[0].name, "log_0.txt");
        EXPECT_FALSE(saves.failures()[0].noFreeName);
        EXPECT_EQ(saves.written(), (std::vector<std::string>{"log_1.txt"}));
    }

    TEST(NumberedSavesTests, NothingToSaveIsDoneAtOnce)
    {
        const NumberedSaves saves{"log", ".txt", 0};
        EXPECT_FALSE(saves.next());
    }
}
